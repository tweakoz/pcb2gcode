/*
 * This file is part of pcb2gcode.
 * 
 * Copyright (C) 2009, 2010 Patrick Birnzain <pbirnzain@users.sourceforge.net>
 * Copyright (C) 2010 Bernhard Kubicek <kubicek@gmx.at>
 * Copyright (C) 2013 Erik Schuster <erik@muenchen-ist-toll.de>
 * Copyright (C) 2014, 2015 Nicola Corna <nicola@corna.info>
 *
 * pcb2gcode is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * pcb2gcode is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with pcb2gcode.  If not, see <http://www.gnu.org/licenses/>.
 */
 
#include <fstream>
#include <cstring>
#include <boost/scoped_array.hpp>
#include <boost/next_prior.hpp>
#include <boost/foreach.hpp>

#include <iostream>
using std::cout;
using std::endl;

#include <sstream>
using std::stringstream;

#include <iomanip>
using std::setprecision;
using std::fixed;

#include <boost/format.hpp>
using boost::format;

#include "drill.hpp"
#include "tsp_solver.hpp"
#include "common.hpp"

using std::pair;
using std::make_pair;
using std::max;
using std::min_element;
using std::cerr;
using std::ios_base;
using std::left;

/******************************************************************************/
/*
 Constructor
 metricoutput : if true, ngc output in metric units
 */
/******************************************************************************/
ExcellonProcessor::ExcellonProcessor(const boost::program_options::variables_map& options,
                                     const icoordpair min,
                                     const icoordpair max)
    : board_width(max.first - min.first),
      board_height(max.second - min.second),
      board_center(min.first + board_width / 2),
      board_minx(min.first),
      drillfront(workSide(options, "drill")),
      mirror_absolute(options["mirror-absolute"].as<bool>()),
      bMetricOutput(options["metricoutput"].as<bool>()),
      quantization_error(2.0 / options["dpi"].as<int>()),
      xoffset(options["zero-start"].as<bool>() ? min.first : 0),
      yoffset(options["zero-start"].as<bool>() ? min.second : 0),
      ocodes(1),
      globalVars(100),
      tileInfo( Tiling::generateTileInfo( options, ocodes, board_height, board_width ) )
{

    bDoSVG = false;      //clear flag for SVG export
    project = gerbv_create_project();

    const char* cfilename = options["drill"].as<string>().c_str();
    boost::scoped_array<char> filename(new char[strlen(cfilename) + 1]);
    strcpy(filename.get(), cfilename);

    gerbv_open_layer_from_filename(project, filename.get());

    if (project->file[0] == NULL)
    {
        throw drill_exception();
    }

    //set imperial/metric conversion factor for output coordinates depending on metricoutput option
    cfactor = bMetricOutput ? 25.4 : 1;

    //set metric or imperial preambles
    if (bMetricOutput)
    {
        preamble = string("G94       (Millimeters per minute feed rate.)\n")
                   + "G21       (Units == Millimeters.)\n";
    }
    else
    {
        preamble = string("G94       (Inches per minute feed rate.)\n")
                   + "G20       (Units == INCHES.)\n";
    }
    preamble += "G90       (Absolute coordinates.)\n";

    tiling = new Tiling( tileInfo, cfactor );
}

/******************************************************************************/
/*
 Destructor
 */
/******************************************************************************/
ExcellonProcessor::~ExcellonProcessor()
{
    gerbv_destroy_project(project);
}

/******************************************************************************/
/*
 */
/******************************************************************************/
void ExcellonProcessor::set_svg_exporter(shared_ptr<SVG_Exporter> svgexpo)
{
    this->svgexpo = svgexpo;
    bDoSVG = true;
}

/******************************************************************************/
/*
 */
/******************************************************************************/
void ExcellonProcessor::add_header(string header)
{
    this->header.push_back(header);
}

/******************************************************************************/
/*
 Recalculates the x-coordinate based on drillfront and mirror_absolute
 drillfront: drill from front side
 mirror_absolute: mirror back side on y-axis
 xvalue: x-coordinate
 returns the recalulated x-coordinate
 */
/******************************************************************************/
double ExcellonProcessor::get_xvalue(double xvalue)
{

    double retval;

    if (drillfront)        //drill from the front, no calculation needed
    {
        retval = xvalue;
    }
    else
    {
        if (mirror_absolute)        //drill from back side, mirrored along y-axis
        {
            retval = (2 * board_minx - xvalue);
        }
        else          //drill from back side, mirrored along board center
        {
            retval = (2 * board_center - xvalue);
        }
    }

    return retval;
}

/******************************************************************************/
/*
 Exports the ngc file for drilling
 of_name: output filename
 driller: ...
 onedrill: if true, only the first drill bit is used, the others are skipped
 
 TODO: 1. Optimise the implementation of onedrill by modifying the bits and using the smallest bit only.
       2. Replace the current tiling implementation (gcode repetition) with a subroutine-based solution
 */
/******************************************************************************/
void ExcellonProcessor::export_ngc( const string of_name, shared_ptr<Driller> driller,
                                    bool onedrill,
                                    bool nog81,
                                    bool swapxy )
{
    ivalue_t double_mirror_axis = mirror_absolute ? 0 : board_width;
    double xoffsetTot;
    double yoffsetTot;
    stringstream zchange;

    //SVG EXPORTER
    int rad = 1.;

    cout << "Exporting drill... ";

    zchange << setprecision(3) << fixed << driller->zchange * cfactor;
    tiling->setGCodeEnd( "G00 Z" + zchange.str() + " ( All done -- retract )\n"
                         + postamble_ext + "\nM5      (Spindle off.)\n"
                         "M9      (Coolant off.)\nM2      (Program end.)\n\n");

    //open output file
    std::ofstream of;
    of.open(of_name.c_str());

    shared_ptr<const map<int, drillbit> > bits = optimise_bits( get_bits(), onedrill );
    shared_ptr<const map<int, icoords> > holes = optimise_path( get_holes(), onedrill );

    //write header to .ngc file
    BOOST_FOREACH (string s, header)
    {
        of << "( " << s << " )" << "\n";
    }

    of << "( Software-independent Gcode )\n";

    if (!onedrill)
    {
        of << "\n( This file uses " << bits->size() << " drill bit sizes. )\n";
        of << "( Bit sizes:";
        for (map<int, drillbit>::const_iterator it = bits->begin();
                it != bits->end(); it++)
        {
            of << " [" << it->second.diameter << it->second.unit << "]";
        }
        of << " )\n\n";
    }
    else
    {
        of << "\n( This file uses only one drill bit. Forced by 'onedrill' option )\n\n";
    }

    of.setf(ios_base::fixed);      //write floating-point values in fixed-point notation
    of.precision(5);           //Set floating-point decimal precision

    of << preamble_ext;        //insert external preamble file
    of << preamble;            //insert internal preamble
    of << "S" << left << driller->speed << "     (RPM spindle speed.)\n" << "\n";

    //tiling->header( of );     // See TODO #2

    for (map<int, drillbit>::const_iterator it = bits->begin();
            it != bits->end(); it++)
    {
        //if the command line option "onedrill" is given, allow only the inital toolchange
        if ((onedrill == true) && (it != bits->begin()))
        {
            of << "(Drill change skipped. Forced by 'onedrill' option.)\n" << "\n";
        }
        else
        {
            of << "G00 Z" << driller->zchange * cfactor << " (Retract)\n" << "T"
               << it->first << "\n" << "M5      (Spindle stop.)\n"
               << "(MSG, Change tool bit to drill size " << it->second.diameter
               << " " << it->second.unit << ")\n"
               << "M6      (Tool change.)\n"
               << "M0      (Temporary machine stop.)\n"
               << "M3      (Spindle on clockwise.)\n" << "\n";
        }
        
        if( nog81 )
            of << "F" << driller->feed * cfactor << endl;
        else
        {
            of << "G81 R" << driller->zsafe * cfactor << " Z"
               << driller->zwork * cfactor << " F" << driller->feed * cfactor << " ";
        }
        
        for( unsigned int i = 0; i < tileInfo.tileY; i++ )
        {
            yoffsetTot = yoffset - i * tileInfo.boardHeight;
            
            for( unsigned int j = 0; j < tileInfo.tileX; j++ )
            {
                xoffsetTot = xoffset - ( i % 2 ? tileInfo.tileX - j - 1 : j ) * tileInfo.boardWidth;

                const icoords drill_coords = holes->at(it->first);
                icoords::const_iterator coord_iter = drill_coords.begin();
                //coord_iter->first = x-coorinate (top view)
                //coord_iter->second =y-coordinate (top view)

                //SVG EXPORTER
                if (bDoSVG)
                {
                    svgexpo->set_rand_color();      //set a random color
                    svgexpo->circle((double_mirror_axis - coord_iter->first),
                                    coord_iter->second, rad);      //draw first circle
                    svgexpo->stroke();
                }

                while (coord_iter != drill_coords.end())
                {
                    if( nog81 )
                    {
                        of << "G0 X"
                           << ( get_xvalue(coord_iter->first) - xoffsetTot ) * cfactor
                           << " Y" << ( ( coord_iter->second - yoffsetTot ) * cfactor) << "\n";
                        of << "G1 Z" << driller->zwork * cfactor << endl;
                        of << "G1 Z" << driller->zsafe * cfactor << endl;
                    }
                    else
                    {
                        auto x = (get_xvalue(coord_iter->first) - xoffsetTot )*cfactor;
                        auto y = (coord_iter->second - yoffsetTot )*cfactor;

                        if( swapxy )
                            of << "X" << y << " Y" << x << "\n";
                        else
                            of << "X" << x << " Y" << y << "\n";
                    }
                    //SVG EXPORTER
                    if (bDoSVG)
                    {
                        svgexpo->circle((double_mirror_axis - coord_iter->first),
                                        coord_iter->second, rad);      //make a whole
                        svgexpo->stroke();
                    }
                    ++coord_iter;
                }
            }
        }
        of << "\n";
    }
    
    //tiling->footer( of ); // See TODO #2
    of << tiling->getGCodeEnd();
    
    of.close();
}

/******************************************************************************/
/*
 *  mill one circle, returns false if tool is bigger than the circle
 */
/******************************************************************************/
bool ExcellonProcessor::millhole(std::ofstream &of, double x, double y,
                                 shared_ptr<Cutter> cutter,
                                 double holediameter)
{

    g_assert(cutter);
    double cutdiameter = cutter->tool_diameter;

    if (cutdiameter * 1.001 >= holediameter)         //In order to avoid a "zero radius arc" error
    {
        of << "G0 X" << x * cfactor << " Y" << y * cfactor << endl;
        of << "G1 Z" << cutter->zwork * cfactor << endl;
        of << "G0 Z" << cutter->zsafe * cfactor << endl << endl;

        return false;
    }
    else
    {

        double millr = (holediameter - cutdiameter) / 2.;      //mill radius

        of << "G0 X" << ( x + millr ) * cfactor << " Y" << y * cfactor << endl;

        double z_step = cutter->stepsize;
        double z = cutter->zwork + z_step * abs(int(cutter->zwork / z_step));

        if (!cutter->do_steps)
        {
            z = cutter->zwork;
            z_step = 1;      //dummy to exit the loop
        }

        int stepcount = abs(int(cutter->zwork / z_step));

        while (z >= cutter->zwork)
        {
            of << "G1 Z[" << cutter->zwork * cfactor << "+" << stepcount << "*" << cutter->stepsize * cfactor << "]" << endl;
            of << "G2 I" << -millr * cfactor << " J0" << endl;
            z -= z_step;
            stepcount--;
        }

        of << "G0 Z" << cutter->zsafe * cfactor << endl << endl;

        return true;
    }
}

/******************************************************************************/
/*
 mill larger holes by using a smaller mill-head
 */
/******************************************************************************/
void ExcellonProcessor::export_ngc(const string outputname, shared_ptr<Cutter> target)
{
    unsigned int badHoles = 0;
    double xoffsetTot;
    double yoffsetTot;
    stringstream zchange;

    //g_assert(drillfront == true);       //WHY?
    //g_assert(mirror_absolute == false); //WHY?
    cout << "Exporting drill... ";

    zchange << setprecision(3) << fixed << target->zchange * cfactor;
    tiling->setGCodeEnd( "G00 Z" + zchange.str() + " ( All done -- retract )\n" +
                         postamble_ext + "\nM5      (Spindle off.)\n"
                         "M9      (Coolant off.)\nM2      (Program end.)\n\n");

    // open output file
    std::ofstream of;
    of.open(outputname.c_str());

    shared_ptr<const map<int, drillbit> > bits = optimise_bits( get_bits(), false );
    shared_ptr<const map<int, icoords> > holes = optimise_path( get_holes(), false );

    // write header to .ngc file
    BOOST_FOREACH (string s, header)
    {
        of << "( " << s << " )" << "\n";
    }

    if( tileInfo.enabled && tileInfo.software != CUSTOM )
        of << "( Gcode for " << getSoftwareString(tileInfo.software) << " )\n";
    else
        of << "( Software-independent Gcode )\n";

    of.setf(ios_base::fixed);      //write floating-point values in fixed-point notation
    of.precision(5);              //Set floating-point decimal precision

    of << "( This file uses a mill head of " << (bMetricOutput ? (target->tool_diameter * 25.4) : target->tool_diameter)
       << (bMetricOutput ? "mm" : "inch") << " to drill the " << bits->size()
       << " bit sizes. )" << "\n";

    of << "( Bit sizes:";
    for (map<int, drillbit>::const_iterator it = bits->begin();
            it != bits->end(); it++)
    {
        of << " [" << it->second.diameter << "]";
    }
    of << " )\n\n";

    //preamble
    of << preamble_ext << preamble << "S" << left << target->speed
       << "    (RPM spindle speed.)\n" << "F" << target->feed * cfactor
       << " (Feedrate)\nM3        (Spindle on clockwise.)\n"
       << "G00 Z" << target->zsafe * cfactor << "\n\n";

    tiling->header( of );

    for( unsigned int i = 0; i < tileInfo.forYNum; i++ )
    {
        yoffsetTot = yoffset - i * tileInfo.boardHeight;
        
        for( unsigned int j = 0; j < tileInfo.forXNum; j++ )
        {
            xoffsetTot = xoffset - ( i % 2 ? tileInfo.forXNum - j - 1 : j ) * tileInfo.boardWidth;

            if( tileInfo.enabled && tileInfo.software == CUSTOM )
                of << "( Piece #" << j + 1 + i * tileInfo.forXNum << ", position [" << j << ";" << i << "] )\n\n";

            for (map<int, drillbit>::const_iterator it = bits->begin();
                    it != bits->end(); it++)
            {

                double diameter = it->second.unit == "mm" ? it->second.diameter / 25.8 : it->second.diameter;

                const icoords drill_coords = holes->at(it->first);
                icoords::const_iterator coord_iter = drill_coords.begin();

                do
                {
                    if( !millhole(of, get_xvalue(coord_iter->first) - xoffsetTot,
                                  coord_iter->second - yoffsetTot, target, diameter) )
                        ++badHoles;

                    ++coord_iter;
                }
                while (coord_iter != drill_coords.end());
            }
        }
    }
    
    tiling->footer( of );

    of.close();

    if( badHoles != 0 )
    {
        badHoles /= tileInfo.tileX * tileInfo.tileY;    //Don't count the same bad hole multiple times
        cerr << "Warning: " << badHoles << ( badHoles == 1 ? " hole was" : " holes were" )
             << " bigger than the milling tool." << endl;
    }
}

/******************************************************************************/
/*
 */
/******************************************************************************/
void ExcellonProcessor::parse_bits()
{
    bits = shared_ptr<map<int, drillbit> >(new map<int, drillbit>());

    for (gerbv_drill_list_t* currentDrill = project->file[0]->image->drill_stats
                                            ->drill_list; currentDrill; currentDrill = currentDrill->next)
    {
        drillbit curBit;
        curBit.diameter = currentDrill->drill_size;
        curBit.unit = string(currentDrill->drill_unit);
        curBit.drill_count = currentDrill->drill_count;

        bits->insert(pair<int, drillbit>(currentDrill->drill_num, curBit));
    }
}

/******************************************************************************/
/*
 */
/******************************************************************************/
void ExcellonProcessor::parse_holes()
{
    if (!bits)
        parse_bits();

    holes = shared_ptr<map<int, icoords> >(new map<int, icoords>());

    for (gerbv_net_t* currentNet = project->file[0]->image->netlist; currentNet;
            currentNet = currentNet->next)
    {
        if (currentNet->aperture != 0)
            (*holes)[currentNet->aperture].push_back(
                icoordpair(currentNet->start_x, currentNet->start_y));
    }

    for (map<int, drillbit>::iterator it = bits->begin(); it != bits->end(); )
        if (holes->count(it->first) == 0)   //If a bit has no associated holes
        {
			cerr << "Warning: bit " << it->first << " (" << it->second.diameter
				 << ' ' << it->second.unit << ") has no associated holes; "
				 "removing it." << std::endl;
            bits->erase(it++);  //remove it
        }
        else
            ++it;
}

/******************************************************************************/
/*
 */
/******************************************************************************/
shared_ptr< map<int, drillbit> > ExcellonProcessor::get_bits()
{
    if (!bits)
        parse_bits();

    return bits;
}

/******************************************************************************/
/*
 */
/******************************************************************************/
shared_ptr< map<int, icoords> > ExcellonProcessor::get_holes()
{
    if (!holes)
        parse_holes();

    return holes;
}

/******************************************************************************/
/*
 Optimisation of the hole path with a TSP Nearest Neighbour algorithm
 */
/******************************************************************************/
shared_ptr< map<int, icoords> > ExcellonProcessor::optimise_path( shared_ptr< map<int, icoords> > original_path, bool onedrill )
{
    unsigned int size = 0;
    map<int, icoords>::iterator i;

    //If the onedrill option has been selected, we can merge all the holes in a single path
    //in order to optimise it even more
    if( onedrill )
    {
        //First find the total number of holes
        for( i = original_path->begin(); i != original_path->end(); i++ )
            size += i->second.size();

        //Then reserve the vector's size
        original_path->begin()->second.reserve( size );

        //Then copy all the paths inside the first and delete the source vector
        map<int, icoords>::iterator second_element;
        while( original_path->size() > 1 )
        {
            second_element = boost::next( original_path->begin() );
            original_path->begin()->second.insert( original_path->begin()->second.end(),
                                                   second_element->second.begin(), second_element->second.end() );
            original_path->erase( second_element );
        }
    }

    //Otimise the holes path
    for( i = original_path->begin(); i != original_path->end(); i++ )
    {
        tsp_solver::nearest_neighbour( i->second, std::make_pair(get_xvalue(0) + xoffset, yoffset), quantization_error );
    }

    return original_path;
}

/******************************************************************************/
/*
 */
/******************************************************************************/
shared_ptr<map<int, drillbit> > ExcellonProcessor::optimise_bits( shared_ptr<map<int, drillbit> > original_bits, bool onedrill )
{
    //The bits optimisation function simply removes all the unnecessary bits when onedrill == true
    if( onedrill )
        original_bits->erase( boost::next( original_bits->begin() ), original_bits->end() );

    return original_bits;
}

/******************************************************************************/
/*
 */
/******************************************************************************/
void ExcellonProcessor::set_preamble(string _preamble)
{
    preamble_ext = _preamble;
}

/******************************************************************************/
/*
 */
/******************************************************************************/
void ExcellonProcessor::set_postamble(string _postamble)
{
    postamble_ext = _postamble;
}
