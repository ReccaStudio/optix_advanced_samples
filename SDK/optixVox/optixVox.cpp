/* 
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//-----------------------------------------------------------------------------
//
// optixGlass: a glass shader example
//
//-----------------------------------------------------------------------------

#ifndef __APPLE__
#  include <GL/glew.h>
#  if defined( _WIN32 )
#    include <GL/wglew.h>
#  endif
#endif

#include <GLFW/glfw3.h>

#include <optixu/optixpp_namespace.h>
#include <optixu/optixu_aabb_namespace.h>
#include <optixu/optixu_math_stream_namespace.h>

#include <sutil.h>
#include "commonStructs.h"
#include "read_vox.h"
#include <Camera.h>

#include <imgui/imgui.h>
#include <imgui/imgui_impl_glfw.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <stdint.h>

using namespace optix;

const char* const SAMPLE_NAME = "optixVox";
const unsigned int WIDTH  = 768u;
const unsigned int HEIGHT = 576u;

//------------------------------------------------------------------------------
//
// Globals
//
//------------------------------------------------------------------------------

Context      context = 0;


//------------------------------------------------------------------------------
//
//  Helper functions
//
//------------------------------------------------------------------------------
    

static std::string ptxPath( const std::string& cuda_file )
{
    return
        std::string(sutil::samplesPTXDir()) +
        "/" + std::string(SAMPLE_NAME) + "_generated_" +
        cuda_file +
        ".ptx";
}


static Buffer getOutputBuffer()
{
    return context[ "output_buffer" ]->getBuffer();
}


void destroyContext()
{
    if( context )
    {
        context->destroy();
        context = 0;
    }
}


void createContext( bool use_pbo )
{
    // Set up context
    context = Context::create();
    context->setRayTypeCount( 1 );
    context->setEntryPointCount( 1 );
    context->setStackSize( 1024 );

    context["max_depth"]->setInt( 2 );
    context["cutoff_color"]->setFloat( 0.2f, 0.2f, 0.2f );
    context["frame"]->setUint( 0u );
    context["scene_epsilon"]->setFloat( 1.e-3f );

    Buffer buffer = sutil::createOutputBuffer( context, RT_FORMAT_UNSIGNED_BYTE4, WIDTH, HEIGHT, use_pbo );
    context["output_buffer"]->set( buffer );

    // Accumulation buffer
    Buffer accum_buffer = context->createBuffer( RT_BUFFER_INPUT_OUTPUT | RT_BUFFER_GPU_LOCAL,
            RT_FORMAT_FLOAT4, WIDTH, HEIGHT );
    context["accum_buffer"]->set( accum_buffer );

    // Ray generation program
    std::string ptx_path( ptxPath( "path_trace_camera.cu" ) );
    Program ray_gen_program = context->createProgramFromPTXFile( ptx_path, "pinhole_camera" );
    context->setRayGenerationProgram( 0, ray_gen_program );

    // Exception program
    Program exception_program = context->createProgramFromPTXFile( ptx_path, "exception" );
    context->setExceptionProgram( 0, exception_program );
    context["bad_color"]->setFloat( 1.0f, 0.0f, 1.0f );

    // Miss program
    ptx_path = ptxPath( "gradientbg.cu" );
    context->setMissProgram( 0, context->createProgramFromPTXFile( ptx_path, "miss" ) );
    context["background_light"]->setFloat( 1.0f, 1.0f, 1.0f );
    context["background_dark"]->setFloat( 0.3f, 0.3f, 0.3f );

    // align background's up direction with camera's look direction
    float3 bg_up = normalize( make_float3(0.0f, -1.0f, -1.0f) );

    // tilt the background's up direction in the direction of the camera's up direction
    bg_up.y += 1.0f;
    bg_up = normalize(bg_up);
    context["up"]->setFloat( bg_up.x, bg_up.y, bg_up.z );
}


Material createDiffuseMaterial()
{
    const std::string ptx_path = ptxPath( "diffuse.cu" );
    Program ch_program = context->createProgramFromPTXFile( ptx_path, "closest_hit_radiance" );

    Material material = context->createMaterial();
    material->setClosestHitProgram( 0, ch_program );

    material["Kd"]->setFloat( make_float3( 0.7f, 0.7f, 0.7f ) );

    return material;
}


optix::Aabb createGeometry(
        const std::string& filename,
        const Material diffuse_material
        )
{
    
    const std::string ptx_path = ptxPath( "boxes.cu" );

    GeometryGroup geometry_group = context->createGeometryGroup();
    geometry_group->setAcceleration( context->createAcceleration( "Trbvh" ) );

    optix::Aabb aabb;

    std::vector< VoxelModel > models;
    optix::uchar4 palette[256];
    try {
        read_vox( filename.c_str(), models, palette );
    } catch ( const std::exception& e ) {
        std::cerr << "Caught exception while reading voxel model: " << filename << std::endl;
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    // Set palette buffer on global context, since it is the same for all models
    {
        Buffer palette_buffer = context->createBuffer( RT_BUFFER_INPUT, RT_FORMAT_UNSIGNED_BYTE4, 256 );
        optix::uchar4* data = static_cast<optix::uchar4*>( palette_buffer->map() );
        for (int i = 0; i < 256; ++i) {
            data[i] = palette[i];    
        }
        palette_buffer->unmap();
        context["palette_buffer"]->set( palette_buffer );
    }
    

    for ( size_t i = 0; i < models.size(); ++i ) {
        const VoxelModel& model = models[i];

        Geometry box_geometry = context->createGeometry();
        const unsigned int num_boxes = (unsigned int)( model.voxels.size() );
        box_geometry->setPrimitiveCount( num_boxes );
        box_geometry->setBoundingBoxProgram( context->createProgramFromPTXFile( ptx_path, "bounds" ) );
        box_geometry->setIntersectionProgram( context->createProgramFromPTXFile( ptx_path, "intersect" ) );

        Buffer box_buffer = context->createBuffer( RT_BUFFER_INPUT, RT_FORMAT_UNSIGNED_BYTE4, num_boxes );
        optix::uchar4* box_data = static_cast<optix::uchar4*>( box_buffer->map());
        for ( unsigned int k = 0; k < num_boxes; ++k ) {
            box_data[k] = model.voxels[k];
        }
        box_buffer->unmap();
        box_geometry["box_buffer"]->set( box_buffer );

        // Compute tight bounds
        optix::uchar4 boxmin = make_uchar4( 255, 255, 255, 255 );
        optix::uchar4 boxmax = make_uchar4( 0, 0, 0, 0 );
        for ( unsigned int k = 0; k < num_boxes; ++k ) {
            boxmin.x = std::min(boxmin.x, model.voxels[k].x);
            boxmin.y = std::min(boxmin.y, model.voxels[k].y);
            boxmin.z = std::min(boxmin.z, model.voxels[k].z);
            boxmax.x = std::max(boxmax.x, model.voxels[k].x);
            boxmax.y = std::max(boxmax.y, model.voxels[k].y);
            boxmax.z = std::max(boxmax.z, model.voxels[k].z);
        }
        aabb.include( 
            make_float3( boxmin.x, boxmin.y, boxmin.z ) / make_float3( 255.0f, 255.0f, 255.0f ),
            make_float3( boxmax.x, boxmax.y, boxmax.z ) / make_float3( 255.0f, 255.0f, 255.0f )
            );

        GeometryInstance instance = context->createGeometryInstance( box_geometry, &diffuse_material, &diffuse_material + 1 );
        geometry_group->addChild( instance );
    }

    context[ "top_object"   ]->set( geometry_group ); 

    return aabb;
}



//------------------------------------------------------------------------------
//
//  GLFW callbacks
//
//------------------------------------------------------------------------------

struct CallbackData
{
    sutil::Camera& camera;
    unsigned int& accumulation_frame;
};

void keyCallback( GLFWwindow* window, int key, int scancode, int action, int mods )
{
    bool handled = false;

    if( action == GLFW_PRESS )
    {
        switch( key )
        {
            case GLFW_KEY_Q:
            case GLFW_KEY_ESCAPE:
                if( context )
                    context->destroy();
                if( window )
                    glfwDestroyWindow( window );
                glfwTerminate();
                exit(EXIT_SUCCESS);

            case( GLFW_KEY_S ):
            {
                const std::string outputImage = std::string(SAMPLE_NAME) + ".png";
                std::cerr << "Saving current frame to '" << outputImage << "'\n";
                sutil::writeBufferToFile( outputImage.c_str(), getOutputBuffer() );
                handled = true;
                break;
            }
            case( GLFW_KEY_F ):
            {
               CallbackData* cb = static_cast<CallbackData*>( glfwGetWindowUserPointer( window ) );
               cb->camera.reset_lookat();
               cb->accumulation_frame = 0;
               handled = true;
               break;
            }
        }
    }

    if (!handled) {
        // forward key event to imgui
        ImGui_ImplGlfw_KeyCallback( window, key, scancode, action, mods );
    }
}

void windowSizeCallback( GLFWwindow* window, int w, int h )
{
    if (w < 0 || h < 0) return;

    const unsigned width = (unsigned)w;
    const unsigned height = (unsigned)h;

    CallbackData* cb = static_cast<CallbackData*>( glfwGetWindowUserPointer( window ) );
    if ( cb->camera.resize( width, height ) ) {
        cb->accumulation_frame = 0;
    }

    sutil::resizeBuffer( getOutputBuffer(), width, height );
    sutil::resizeBuffer( context[ "accum_buffer" ]->getBuffer(), width, height );

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1);
    glViewport(0, 0, width, height);
}


//------------------------------------------------------------------------------
//
// GLFW setup and run 
//
//------------------------------------------------------------------------------

GLFWwindow* glfwInitialize( )
{
    GLFWwindow* window = sutil::initGLFW();

    // Note: this overrides imgui key callback with our own.  We'll chain this.
    glfwSetKeyCallback( window, keyCallback );

    glfwSetWindowSize( window, (int)WIDTH, (int)HEIGHT );
    glfwSetWindowSizeCallback( window, windowSizeCallback );

    return window;
}


void glfwRun( GLFWwindow* window, sutil::Camera& camera )
{
    // Initialize GL state
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1 );
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glViewport(0, 0, WIDTH, HEIGHT );

    unsigned int frame_count = 0;
    unsigned int accumulation_frame = 0;
    int max_depth = 10;

    // Expose user data for access in GLFW callback functions when the window is resized, etc.
    // This avoids having to make it global.
    CallbackData cb = { camera, accumulation_frame };
    glfwSetWindowUserPointer( window, &cb );

    while( !glfwWindowShouldClose( window ) )
    {

        glfwPollEvents();                                                        

        ImGui_ImplGlfw_NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        
        // Let imgui process the mouse first
        if (!io.WantCaptureMouse) {

            double x, y;
            glfwGetCursorPos( window, &x, &y );

            if ( camera.process_mouse( (float)x, (float)y, ImGui::IsMouseDown(0), ImGui::IsMouseDown(1), ImGui::IsMouseDown(2) ) ) {
                accumulation_frame = 0;
            }
        }

        // imgui pushes
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(0,0) );
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,          0.6f        );
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f        );

        sutil::displayFps( frame_count++ );

#if 0
        {
            static const ImGuiWindowFlags window_flags = 
                    ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoScrollbar;

            ImGui::SetNextWindowPos( ImVec2( 2.0f, 40.0f ) );
            ImGui::Begin("controls", 0, window_flags );
            if (ImGui::SliderFloat3( "extinction", (float*)(&glass_extinction.x), 0.01f, 1.0f )) {
                context["extinction_constant"]->setFloat( log(glass_extinction.x), log(glass_extinction.y), log(glass_extinction.z) );
                accumulation_frame = 0;
            }
            if (ImGui::SliderInt( "max depth", &max_depth, 1, 10 )) {
                context["max_depth"]->setInt( max_depth );
                accumulation_frame = 0;
            }
            ImGui::End();
        }
#endif

        // imgui pops
        ImGui::PopStyleVar( 3 );

        // Render main window
        context["frame"]->setUint( accumulation_frame++ );
        context->launch( 0, camera.width(), camera.height() );
        sutil::displayBufferGL( getOutputBuffer() );

        // Render gui over it
        ImGui::Render();

        glfwSwapBuffers( window );
    }
    
    destroyContext();
    glfwDestroyWindow( window );
    glfwTerminate();
}


//------------------------------------------------------------------------------
//
// Main
//
//------------------------------------------------------------------------------

void printUsageAndExit( const std::string& argv0 )
{
    std::cerr << "\nUsage: " << argv0 << " [options] file.vox\n";
    std::cerr <<
        "App Options:\n"
        "  -h | --help                  Print this usage message and exit.\n"
        "  -f | --file <output_file>    Save image to file and exit.\n"
        "  -n | --nopbo                 Disable GL interop for display buffer.\n"
        "App Keystrokes:\n"
        "  q  Quit\n"
        "  s  Save image to '" << SAMPLE_NAME << ".png'\n"
        "  f  Re-center camera\n"
        "\n"
        << std::endl;

    exit(1);
}


int main( int argc, char** argv )
{
    bool use_pbo  = true;
    std::string out_file;
    std::string vox_file;
    for( int i=1; i<argc; ++i )
    {
        const std::string arg( argv[i] );

        if( arg == "-h" || arg == "--help" )
        {
            printUsageAndExit( argv[0] );
        }
        else if( arg == "-f" || arg == "--file"  )
        {
            if( i == argc-1 )
            {
                std::cerr << "Option '" << arg << "' requires additional argument.\n";
                printUsageAndExit( argv[0] );
            }
            out_file = argv[++i];
        }
        else if( arg == "-n" || arg == "--nopbo"  )
        {
            use_pbo = false;
        }
        else if( arg[0] == '-' )
        {
            std::cerr << "Unknown option '" << arg << "'\n";
            printUsageAndExit( argv[0] );
        }
        else {
            // Interpret argument as a mesh file.
            vox_file = std::string( argv[i] );
        }
    }

    try
    {
        GLFWwindow* window = glfwInitialize();

#ifndef __APPLE__
        GLenum err = glewInit();
        if (err != GLEW_OK)
        {
            std::cerr << "GLEW init failed: " << glewGetErrorString( err ) << std::endl;
            exit(EXIT_FAILURE);
        }
#endif

        createContext( use_pbo );

        if ( vox_file.empty() ) {

            // Default scene
            vox_file = std::string( sutil::samplesDir() ) + "/data/monu1.vox";
        }

        Material material = createDiffuseMaterial();
        const optix::Aabb aabb = createGeometry( vox_file, material );

        // Note: lighting comes from miss program

        context->validate();

        sutil::Camera camera( WIDTH, HEIGHT, 
                optix::make_float3( 0.0f, 1.5f*aabb.extent(1), 1.5f*aabb.extent(2) ),
                aabb.center(),  // lookat
                make_float3( 0.0f, 1.0f,  0.0f ),    //up
                context["eye"], context["U"], context["V"], context["W"] );

        if ( out_file.empty() )
        {
            glfwRun( window, camera );
        }
        else
        {
            // Accumulate frames for anti-aliasing
            const unsigned int numframes = 256;
            std::cerr << "Accumulating " << numframes << " frames ..." << std::endl;
            for ( unsigned int frame = 0; frame < numframes; ++frame ) {
                context["frame"]->setUint( frame );
                context->launch( 0, WIDTH, HEIGHT );
            }
            sutil::writeBufferToFile( out_file.c_str(), getOutputBuffer() );
            std::cerr << "Wrote " << out_file << std::endl;
            destroyContext();
        }
        return 0;
    }
    SUTIL_CATCH( context->get() )
}
