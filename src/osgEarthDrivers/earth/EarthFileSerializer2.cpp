/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2015 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include "EarthFileSerializer"
#include <osgEarth/FileUtils>
#include <osgEarth/MapFrame>
#include <osgEarth/Extension>
#include <osgEarth/StringUtils>
#include <osgEarth/FileUtils>
#include <osgDB/FileUtils>
#include <osgDB/FileNameUtils>
#include <stdio.h>
#include <ctype.h>

using namespace osgEarth_osgearth;
using namespace osgEarth;

#undef  LC
#define LC "[EarthSerializer2] "

namespace
{
    /**
     * Looks at each key in a Config and tries to match that key to a shared library name;
     * loads the shared library associated with the name. This will "pre-load" all the DLLs
     * associated with extensions in the earth file even if they weren't linked.
     *
     * Will also pre-load any expressly indicated shared libraries in the "libraries" element.
     */
    void preloadExtensionLibs(const Config& conf)
    {
        ConfigSet extensions = conf.child("extensions").children();
        for(ConfigSet::const_iterator i = extensions.begin(); i != extensions.end(); ++i)
        {
            const std::string& name = i->key();

            // Load the extension library if necessary.
            std::string libName = osgDB::Registry::instance()->createLibraryNameForExtension("osgearth_" + name);
            osgDB::Registry::LoadStatus status = osgDB::Registry::instance()->loadLibrary(libName);
            if ( status == osgDB::Registry::LOADED )
            {
                OE_INFO << LC << "Loaded extension lib \"" << libName << "\"\n";
            }
            else
            {
                // If it failed to load, try loading an extension from an osgEarth library with the same name.
                // Capitalize the name of the extension,.
                std::string capName = name;
                capName[0] = ::toupper(capName[0]);
                std::stringstream buf;
                buf << "osgEarth" << capName;
                libName = osgDB::Registry::instance()->createLibraryNameForNodeKit(buf.str());
                status = osgDB::Registry::instance()->loadLibrary(libName);
                if (status == osgDB::Registry::LOADED)
                {
                    OE_INFO << LC << "Loaded extension lib \"" << libName << "\"\n";
                }
            }
        }

        // Preload any libraries
        Config libraries = conf.child("libraries");
        if (!libraries.value().empty())
        {
            StringTokenizer izer( ";" );
            StringVector libs;
            izer.tokenize( libraries.value(), libs );
            for (StringVector::iterator itr = libs.begin(); itr != libs.end(); ++itr)
            {
                std::string lib = *itr;
                trim2(lib);
                std::string libName = osgDB::Registry::instance()->createLibraryNameForNodeKit(lib);
                osgDB::Registry::LoadStatus status = osgDB::Registry::instance()->loadLibrary(libName);
                if (status == osgDB::Registry::LOADED)
                {
                    OE_INFO << LC << "Loaded library \"" << libName << "\"\n";
                }
                else
                {
                    OE_WARN << LC << "Failed to load library \"" << libName << "\"\n";
                }
            }
        }        
    }

    /**
     * Visits a Config hierarchy and rewrites relative pathnames to be relative to a new referrer.
     */
    struct RewritePaths
    {
        bool        _rewriteAbsolutePaths;
        std::string _newReferrerAbsPath;
        std::string _newReferrerFolder;

        RewritePaths(const std::string& referrer)
        {
            _rewriteAbsolutePaths = false;
            _newReferrerAbsPath = osgDB::convertFileNameToUnixStyle( osgDB::getRealPath(referrer) );
            _newReferrerFolder  = osgDB::getFilePath( osgDB::findDataFile(_newReferrerAbsPath) );
        }

        /** Whether to make absolute paths into relative paths if possible */
        void setRewriteAbsolutePaths(bool value)
        {
            _rewriteAbsolutePaths = value;
        }

        void apply(Config& input)
        {
            // only consider "simple" values (no children) with a set referrer:
            if ( !input.referrer().empty() && input.isSimple() )
            {
                // If the input has a referrer set, it might be a path. Rewrite the path
                // to be relative to the new referrer that was passed into this visitor.

                // resolve the absolute path of the input:
                URI inputURI( input.value(), URIContext(input.referrer()) );
                std::string inputAbsPath = osgDB::convertFileNameToUnixStyle( inputURI.full() );

                // see whether the file exists (this is how we verify that it's actually a path)
                if ( osgDB::fileExists(inputAbsPath) )
                {
                    if ( !osgDB::isAbsolutePath(input.value()) || _rewriteAbsolutePaths )
                    {
                        std::string inputNewRelPath = osgDB::getPathRelative( _newReferrerFolder, inputAbsPath );
                    
                        OE_DEBUG << LC << "\n"
                            "   Rewriting \"" << input.value() << "\" as \"" << inputNewRelPath << "\"\n"
                            "   Absolute = " << inputAbsPath << "\n"
                            "   ReferrerFolder = " << _newReferrerFolder << "\n";

                        if ( input.value() != inputNewRelPath )
                        {
                            input.value() = inputNewRelPath;
                            input.setReferrer( _newReferrerAbsPath );
                        }
                    }
                }
            }

            for(ConfigSet::iterator i = input.children().begin(); i != input.children().end(); ++i)
            {
                apply( *i );
            }
        }
    };
}

//............................................................................


MapNode*
EarthFileSerializer2::deserialize( const Config& conf, const std::string& referrer ) const
{
    // First, pre-load any extension DLLs.
    preloadExtensionLibs(conf);

    MapOptions mapOptions( conf.child( "options" ) );

    // legacy: check for name/type in top-level attrs:
    if ( conf.hasValue( "name" ) || conf.hasValue( "type" ) )
    {
        Config legacy;
        if ( conf.hasValue("name") ) legacy.add( "name", conf.value("name") );
        if ( conf.hasValue("type") ) legacy.add( "type", conf.value("type") );
        mapOptions.mergeConfig( legacy );
    }

    Map* map = new Map( mapOptions );

    // Yes, MapOptions and MapNodeOptions share the same Config node. Weird but true.
    MapNodeOptions mapNodeOptions( conf.child( "options" ) );

    // Read the layers in LAST (otherwise they will not benefit from the cache/profile configuration)

    // Image layers:
    ConfigSet images = conf.children( "image" );
    for( ConfigSet::const_iterator i = images.begin(); i != images.end(); i++ )
    {
        Config layerDriverConf = *i;        

        ImageLayerOptions layerOpt( layerDriverConf );
        layerOpt.name() = layerDriverConf.value("name");

        map->addImageLayer( new ImageLayer(layerOpt) );
    }

    // Elevation layers:
    for( int k=0; k<2; ++k )
    {
        std::string tagName = k == 0 ? "elevation" : "heightfield"; // support both :)

        ConfigSet heightfields = conf.children( tagName );
        for( ConfigSet::const_iterator i = heightfields.begin(); i != heightfields.end(); i++ )
        {
            Config layerDriverConf = *i;            

            ElevationLayerOptions layerOpt( layerDriverConf );
            layerOpt.name() = layerDriverConf.value( "name" );

            map->addElevationLayer( new ElevationLayer(layerOpt) );
        }
    }

    // Model layers:
    ConfigSet models = conf.children( "model" );
    for( ConfigSet::const_iterator i = models.begin(); i != models.end(); i++ )
    {
        const Config& layerDriverConf = *i;

        ModelLayerOptions layerOpt( layerDriverConf );
        layerOpt.name() = layerDriverConf.value( "name" );
        layerOpt.driver() = ModelSourceOptions( layerDriverConf );

        map->addModelLayer( new ModelLayer(layerOpt) );
    }

    // Mask layer:
    ConfigSet masks = conf.children( "mask" );
    for( ConfigSet::const_iterator i = masks.begin(); i != masks.end(); i++ )
    {
        Config maskLayerConf = *i;

        MaskLayerOptions options(maskLayerConf);
        options.name() = maskLayerConf.value( "name" );
        options.driver() = MaskSourceOptions(options);

        map->addTerrainMaskLayer( new MaskLayer(options) );
    }

    
    //Add any addition paths specified in the options/osg_file_paths element to the file path.  Useful for pointing osgEarth at resource folders.
    Config osg_file_paths = conf.child( "options" ).child("osg_file_paths");
    ConfigSet urls = osg_file_paths.children("url");
    for (ConfigSet::const_iterator i = urls.begin(); i != urls.end(); i++) 
    {
        std::string path = osgEarth::getFullPath( referrer, (*i).value());
        OE_DEBUG << "Adding OSG file path " << path << std::endl;
        osgDB::Registry::instance()->getDataFilePathList().push_back( path );
    }

    osg::ref_ptr<MapNode> mapNode = new MapNode( map, mapNodeOptions );

    // External configs. Support both "external" and "extensions" tags.

    Config ext = conf.child( "external" );
    if ( ext.empty() )
        ext = conf.child( "extensions" );

    if ( !ext.empty() )
    {
        // save the configuration in case we need to write it back out later
        mapNode->externalConfig() = ext;

        // locate and install any registered extensions.
        ConfigSet extensions = ext.children();
        for(ConfigSet::const_iterator i = extensions.begin(); i != extensions.end(); ++i)
        {
            Extension* extension = Extension::create( i->key(), *i );
            if ( extension )
            {
                mapNode->addExtension( extension );
            }
        }
    }

    return mapNode.release();
}


Config
EarthFileSerializer2::serialize(const MapNode* input, const std::string& referrer) const
{
    Config mapConf("map");
    mapConf.set("version", "2");

    if ( !input || !input->getMap() )
        return mapConf;

    const Map* map = input->getMap();
    MapFrame mapf( map, Map::ENTIRE_MODEL );

    // the map and node options:
    Config optionsConf = map->getInitialMapOptions().getConfig();
    optionsConf.merge( input->getMapNodeOptions().getConfig() );
    mapConf.add( "options", optionsConf );

    // the layers
    for( ImageLayerVector::const_iterator i = mapf.imageLayers().begin(); i != mapf.imageLayers().end(); ++i )
    {
        ImageLayer* layer = i->get();
        //Config layerConf = layer->getInitialOptions().getConfig();
        Config layerConf = layer->getImageLayerOptions().getConfig();
        layerConf.set("name", layer->getName());
        layerConf.set("driver", layer->getInitialOptions().driver()->getDriver());        
        mapConf.add( "image", layerConf );
    }

    for( ElevationLayerVector::const_iterator i = mapf.elevationLayers().begin(); i != mapf.elevationLayers().end(); ++i )
    {
        ElevationLayer* layer = i->get();
        //Config layerConf = layer->getInitialOptions().getConfig();
        Config layerConf = layer->getElevationLayerOptions().getConfig();
        layerConf.set("name", layer->getName());
        layerConf.set("driver", layer->getInitialOptions().driver()->getDriver());        
        mapConf.add( "elevation", layerConf );
    }

    for( ModelLayerVector::const_iterator i = mapf.modelLayers().begin(); i != mapf.modelLayers().end(); ++i )
    {
        ModelLayer* layer = i->get();
        Config layerConf = layer->getModelLayerOptions().getConfig();
        layerConf.set("name", layer->getName());
        layerConf.set("driver", layer->getModelLayerOptions().driver()->getDriver());
        mapConf.add( "model", layerConf );
    }

    Config ext = input->externalConfig();
    if ( !ext.empty() )
    {
        ext.key() = "extensions";
        mapConf.add( ext );
    }

    // visit the Config to find nodes with a referrer set.
    if ( !referrer.empty() )
    {
        RewritePaths rewritePaths( referrer );
        rewritePaths.apply( mapConf );
    }

    return mapConf;
}
