
/* Copyright (c) 2014-2016, EPFL/Blue Brain Project
 *                          Stefan.Eilemann@epfl.ch
 *                          Daniel.Nachbaur@epfl.ch
 *
 * This file is part of Fivox <https://github.com/BlueBrain/Fivox>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 3.0 as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "eventSource.h"
#include "uriHandler.h"
#include <fivox/version.h>

#include <lunchbox/atomic.h>
#include <lunchbox/debug.h>
#include <lunchbox/log.h>
#include <lunchbox/memoryMap.h>

#include <fstream>

#ifdef USE_BOOST_GEOMETRY
#  include <lunchbox/lock.h>
#  include <lunchbox/scopedMutex.h>
#  include <boost/geometry.hpp>
#  include <boost/geometry/geometries/box.hpp>
#  include <boost/geometry/geometries/point.hpp>
#  include <boost/geometry/index/rtree.hpp>

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

typedef bg::model::point< float, 3, bg::cs::cartesian > Point;
typedef bg::model::box< Point > Box;
typedef std::pair< Point, size_t > Value;
typedef std::vector< Value > Values;

#endif

namespace
{

const size_t maxElemInNode = 64;
const size_t minElemInNode = 16;
const uint32_t magic = 0xfebf;
const uint32_t version = 1;

size_t _getBinarySize( const size_t numEvents )
{
    return numEvents * 5 * sizeof( float ) +
           sizeof( magic ) + sizeof( version );
}

}

namespace fivox
{

class EventSource::Impl
{
public:
    enum EventOffsets
    {
        POSX = 0,
        POSY,
        POSZ,
        RADIUS,
        VALUE,
        NUM_OFFSETS
    };

    explicit Impl( const URIHandler& params )
        : dt( params.getDt( ))
        , duration( params.getDuration( ))
        , currentTime( -1.f )
        , cutOffDistance( params.getCutoffDistance( ))
        , alignBoundary( 32 )
        , numEvents( 0 )
        , allocSize( 0 )
    {}

    void resize( const size_t numEvents_ )
    {
        numEvents = numEvents_;
        if( numEvents_ < allocSize )
            return;

        allocSize = numEvents_;
        const size_t size = numEvents * EventOffsets::NUM_OFFSETS;
        void* ptr;
        if( posix_memalign( &ptr, alignBoundary, size * sizeof(float) ))
        {
            LBWARN << "Memory alignment failed. "
                   << "Trying normal allocation" << std::endl;
            ptr = calloc( size, sizeof(float) );
            if( !ptr )
                LBTHROW( std::bad_alloc( ));
        }
        events.reset((float*) ptr );
    }

    bool readAscii( const std::string& filename )
    {
        std::string line;
        std::ifstream file( filename );

        if( !file.is_open( ))
            return false;

        size_t index = 0;
        while( std::getline( file, line ))
        {
            if( line[0] == '#' || line[0] == '\n' )
                continue;

            if( line.find( "Number of events: ") != std::string::npos )
            {
                const auto pos = line.find_last_of( " \t" );
                resize( atoi( line.substr( pos + 1 ).c_str( )));
                continue;
            }

            if( numEvents == 0 )
            {
                LBWARN << "No events to load. Please check that the number "
                          "of events in the specified file is > 0" << std::endl;
                return false;
            }

            // split the line in tokens (separated by white spaces)
            std::vector< float > event;
            std::string token;
            std::istringstream iss( line );
            while( iss >> token )
                event.push_back( atof( token.c_str( )));

            if( event.size() != 5 )
            {
                LBWARN << "Error while reading " +
                          std::to_string( numEvents ) +
                          " events from file: event " + std::to_string( index )
                          + " ill-formed." << std::endl;
                return false;
            }

            update( index++, Vector3f( event[0], event[1], event[2] ),
                    event[3], event[4] );
        }
        file.close();
        LBINFO << "Loaded " << numEvents << " events from ASCII file "
               << filename << std::endl;

        return file.good();
    }

    bool isBinary( const lunchbox::MemoryMap& binaryFile ) const
    {
        const uint32_t* iData = binaryFile.getAddress< uint32_t >();
        return iData[ 0 ] == magic;
    }

    bool readBinary( const std::string& filename )
    {
        lunchbox::MemoryMap binaryFile( filename );

        const size_t size = binaryFile.getSize();

        const size_t nElems = size / sizeof( uint32_t );
        if( nElems == 0 )
        {
            LBWARN << filename + " is empty" << std::endl;
            return false;
        }

        const uint32_t* iData = binaryFile.getAddress< uint32_t >();
        const float* fData = binaryFile.getAddress< float >();

        if( !isBinary( binaryFile ))
            return false;

        size_t index = 1;
        if( index >= nElems || iData[ index++ ] != version )
        {
            LBWARN << "Bad version in " + filename << std::endl;
            return false;
        }

        const size_t numEvents_ = ( nElems - index ) / 5;
        if( _getBinarySize( numEvents_ ) < size )
        {
            LBWARN << "Error while reading " + std::to_string( numEvents_ ) +
                      " events from file." << std::endl;
            return false;
        }

        resize( numEvents_ );
        for( size_t i = 0; i < numEvents_; ++i )
        {
            const Vector3f pos( fData[ index ],
                                fData[ index + 1 ],
                                fData[ index + 2 ]);
            index += 3;
            const float radius( fData[ index++ ]);
            const float value( fData[ index++ ]);
            update( i, pos, radius, value );
        }

        LBINFO << "Loaded " << numEvents_ << " events from binary file "
               << filename << std::endl;
        return true;
    }

    const float* getPositionsX() const
    {
        return events.get() + numEvents * EventOffsets::POSX;
    }

    const float* getPositionsY() const
    {
        return events.get() + numEvents * EventOffsets::POSY;
    }

    const float* getPositionsZ() const
    {
        return events.get() + numEvents * EventOffsets::POSZ;
    }

    const float* getRadii() const
    {
        return events.get() + numEvents * EventOffsets::RADIUS;
    }

    const float* getValues() const
    {
        return events.get() + numEvents * EventOffsets::VALUE;
    }

    void update( const size_t i, const Vector3f& pos,
                 const float rad, const float val )
    {
        const size_t size( numEvents );
        if( size <= i )
        {
            LBWARN << "The specified index is not valid. Event not added"
                   << std::endl;
            return;
        }

        boundingBox.merge( pos );
        events.get()[ i + size * Impl::EventOffsets::POSX ] = pos[0];
        events.get()[ i + size * Impl::EventOffsets::POSY ] = pos[1];
        events.get()[ i + size * Impl::EventOffsets::POSZ ] = pos[2];

        // radius is inverted to improve performance at computing time
        // e.g. LFP functor
        if( std::abs( rad ) > std::numeric_limits< float >::epsilon( )) // rad != 0
            events.get()[i + size * Impl::EventOffsets::RADIUS] =  1.f / rad;

        events.get()[ i + size * Impl::EventOffsets::VALUE ] = val;

    #ifdef USE_BOOST_GEOMETRY
        rtree.clear();
    #endif
    }

    float dt;
    float duration;
    float currentTime;
    const float cutOffDistance;

    const size_t alignBoundary;
    size_t numEvents;
    size_t allocSize;
    Events events;
    AABBf boundingBox;

#ifdef USE_BOOST_GEOMETRY
    typedef bgi::rtree< Value, bgi::rstar< maxElemInNode, minElemInNode > > RTree;
    RTree rtree;

    void buildRTree()
    {
        if( !rtree.empty( ))
            return;

        LBINFO << "Building rtree for " << numEvents << " events"
               << std::endl;
        Values positions;
        positions.reserve( numEvents );

        for( size_t i = 0; i < numEvents; i++ )
        {
            const Point point( getPositionsX()[i],
                               getPositionsY()[i],
                               getPositionsZ()[i] );
            positions.push_back( std::make_pair( point, i ));
        }

        RTree rt( positions.begin(), positions.end( ));
        rtree = boost::move( rt );
        LBINFO << " done" << std::endl;
    }
#endif
};

EventSource::EventSource( const URIHandler& params )
    : _impl( new EventSource::Impl( params ))
{}

EventSource::~EventSource()
{}

float& EventSource::operator[]( const size_t index )
{
    return _impl->events.get()[ _impl->numEvents * Impl::EventOffsets::VALUE +
                                index ];
}

size_t EventSource::getNumEvents() const
{
    return _impl->numEvents;
}

const float* EventSource::getPositionsX() const
{
    return _impl->getPositionsX();
}

const float* EventSource::getPositionsY() const
{
    return _impl->getPositionsY();
}

const float* EventSource::getPositionsZ() const
{
    return _impl->getPositionsZ();
}

const float* EventSource::getRadii() const
{
    return _impl->getRadii();
}

const float* EventSource::getValues() const
{
    return _impl->getValues();
}

EventValues EventSource::findEvents( const AABBf& area LB_UNUSED ) const
{
    EventValues eventValues;
#ifdef USE_BOOST_GEOMETRY
    if( !_impl->rtree.empty( ))
    {
        const Vector3f& p1 = area.getMin();
        const Vector3f& p2 = area.getMax();
        const Box query( Point( p1[0], p1[1], p1[2] ),
                         Point( p2[0], p2[1], p2[2] ));

        static lunchbox::a_ssize_t maxHits( 0 );
        std::vector< Value > hits;
        hits.reserve( maxHits );
        _impl->rtree.query( bgi::intersects( query ), std::back_inserter( hits ));
        maxHits = std::max( size_t(maxHits), hits.size( ));

        eventValues.reserve( hits.size( ));
        for( const Value& value : hits )
            eventValues.push_back( getValues()[value.second] );
    }
    else
#endif
    // return empty
    {
        static bool first = true;
        if( first )
        {
            LBWARN << "RTree not available for findEvents. "
                   << "No events will be returned" << std::endl;
            first = false;
        }
    }
    return eventValues;
}

void EventSource::setBoundingBox( const AABBf& boundingBox )
{
    _impl->boundingBox = boundingBox;
}

const AABBf& EventSource::getBoundingBox() const
{
    return _impl->boundingBox;
}

float EventSource::getCutOffDistance() const
{
    return _impl->cutOffDistance;
}

void EventSource::resize( const size_t size )
{
    _impl->resize( size );
}

void EventSource::update( const size_t i, const Vector3f& pos,
                          const float rad, const float val )
{
    _impl->update( i, pos, rad, val );
}

void EventSource::buildRTree()
{
#ifdef USE_BOOST_GEOMETRY
    _impl->buildRTree();
#endif
}

bool EventSource::setFrame( const uint32_t frame )
{
    if( !isInFrameRange( frame ))
        return false;

    const float time  = _getTimeRange().x() + getDt() * frame;
    setTime( time );
    return true;
}

void EventSource::setTime( const float time )
{
    _impl->currentTime = time;
}

Vector2ui EventSource::getFrameRange() const
{
    const Vector2f& interval = _getTimeRange();
    switch( _getType( ))
    {
    case SourceType::event:
    {
        const float endTime = interval.y( ) - getDuration();
        if( endTime < interval.x( ))
            return Vector2ui( 0, 0 );
        // If the source end time is t it means that the frame starting and
        // floor(t/dt) is complete, we add 1 frame because the frame range is
        // open on the right.
        return Vector2ui( std::floor( interval.x() / getDt( )),
                          std::floor( endTime / getDt( )) + 1);
    }
    case SourceType::frame:
    default:
        return Vector2ui( std::floor( interval.x() / getDt( )),
                          std::ceil( interval.y() / getDt( )));
    }
}

bool EventSource::isInFrameRange( const uint32_t frame )
{
    const Vector2ui& frameRange = getFrameRange();
    return frame >= frameRange[0] && frame < frameRange[1];
}

float EventSource::getDt() const
{
    return _impl->dt;
}

void EventSource::setDt( const float dt )
{
    _impl->dt = dt;
}

float EventSource::getDuration() const
{
    return _impl->duration;
}

float EventSource::getCurrentTime() const
{
    return _impl->currentTime;
}

ssize_t EventSource::load( const size_t chunkIndex, const size_t numChunks )
{
    if( numChunks == 0 )
        LBTHROW( std::runtime_error(
                     "EventSource::load: numChunks must be > 0" ));
    if( chunkIndex + numChunks > getNumChunks( ))
        LBTHROW( std::out_of_range( "EventSource::load: Out of range" ));
    return _load( chunkIndex, numChunks );
}

ssize_t EventSource::load()
{
    return load( 0, getNumChunks( ));
}

size_t EventSource::getNumChunks() const
{
    return _getNumChunks();
}

bool EventSource::read( const std::string& filename )
{
    if( _impl->readBinary( filename ))
        return true;

    return _impl->readAscii( filename );
}

bool EventSource::write( const std::string& filename,
                         const EventFileFormat format ) const
{
    const size_t numEvents = getNumEvents();
    switch( format )
    {
    case EventFileFormat::binary:
    {
        const size_t size = _getBinarySize( numEvents );

        lunchbox::MemoryMap file( filename, size );
        uint32_t* iData = file.getAddress< uint32_t >();
        float* fData = file.getAddress< float >();
        size_t index = 0;

        iData[ index++ ] = magic;
        iData[ index++ ] = version;
        for( size_t i = 0; i < numEvents; ++i )
        {
            fData[ index++ ] = getPositionsX()[i];
            fData[ index++ ] = getPositionsY()[i];
            fData[ index++ ] = getPositionsZ()[i];
            fData[ index++ ] = getRadii()[i];
            fData[ index++ ] = getValues()[i];
        }
        LBINFO << "Events file written as " << filename << std::endl;
        return true;
    }
    case EventFileFormat::ascii:
    {
        std::ofstream file( filename.c_str( ));
        if( file.is_open( ))
        {
            file << "# Fivox events (3D position, radius and value), "
                 << "in the following format:\n"
                 << "#     posX posY posZ radius value\n"
                 << "# File version: 1\n"
                 << "# Fivox version: " << fivox::Version::getString() << "\n"
                 << "Number of events: " << numEvents
                 << std::endl;

            for( size_t i = 0; i < numEvents; ++i )
            {
                file << getPositionsX()[i] << " "
                     << getPositionsY()[i] << " "
                     << getPositionsZ()[i] << " "
                     << getRadii()[i] << " " << getValues()[i] << std::endl;
            }
            if( file.good( ))
                LBINFO << "Events file written as " << filename << std::endl;
        }
        return file.good();
    }
    default:
        return false;
    }
}
}
