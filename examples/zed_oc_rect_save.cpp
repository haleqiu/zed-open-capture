///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2021, STEREOLABS.
//
// All rights reserved.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////

// ----> Includes
#include <iostream>
#include <sstream>
#include <string>
#include <filesystem>

#include "videocapture.hpp"

// OpenCV includes
#include <opencv2/opencv.hpp>

// Sample includes
#include "calibration.hpp"
#include "ocv_display.hpp"
// <---- Includes

// ----> Global functions
int main(int argc, char *argv[])
{
    // Remove the unused warning silencing since we'll use argc/argv
    if(argc != 2)
    {
        std::cout << "Usage: " << argv[0] << " <output_directory>" << std::endl;
        return EXIT_FAILURE;
    }

    // Create output directory and subdirectories if they don't exist
    std::string output_dir = argv[1];
    std::string left_dir = output_dir + "/left";
    std::string right_dir = output_dir + "/right";
    std::filesystem::create_directories(left_dir);
    std::filesystem::create_directories(right_dir);

    sl_oc::VERBOSITY verbose = sl_oc::VERBOSITY::INFO;

    // ----> Set Video parameters
    sl_oc::video::VideoParams params;
    params.res = sl_oc::video::RESOLUTION::HD720;
    params.fps = sl_oc::video::FPS::FPS_60;
    params.verbose = verbose;
    // <---- Set Video parameters

    // ----> Create Video Capture
    sl_oc::video::VideoCapture cap(params);
    if( !cap.initializeVideo(-1) )
    {
        std::cerr << "Cannot open camera video capture" << std::endl;
        std::cerr << "See verbosity level for more details." << std::endl;

        return EXIT_FAILURE;
    }
    int sn = cap.getSerialNumber();
    std::cout << "Connected to camera sn: " << sn << std::endl;
    // <---- Create Video Capture

    // ----> Retrieve calibration file from Stereolabs server
    std::string calibration_file;
    // ZED Calibration
    unsigned int serial_number = sn;
    // Download camera calibration file
    if( !sl_oc::tools::downloadCalibrationFile(serial_number, calibration_file) )
    {
        std::cerr << "Could not load calibration file from Stereolabs servers" << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "Calibration file found. Loading..." << std::endl;

    // ----> Frame size
    int w,h;
    cap.getFrameSize(w,h);
    // <---- Frame size

    // ----> Initialize calibration
    cv::Mat map_left_x, map_left_y;
    cv::Mat map_right_x, map_right_y;
    cv::Mat cameraMatrix_left, cameraMatrix_right;
    sl_oc::tools::initCalibration(calibration_file, cv::Size(w/2,h), map_left_x, map_left_y, map_right_x, map_right_y,
                    cameraMatrix_left, cameraMatrix_right);

    std::cout << " Camera Matrix L: \n" << cameraMatrix_left << std::endl << std::endl;
    std::cout << " Camera Matrix R: \n" << cameraMatrix_right << std::endl << std::endl;
    // ----> Initialize calibration

    cv::Mat frameBGR, left_raw, left_rect, right_raw, right_rect;

    uint64_t last_ts=0;

    // Infinite video grabbing loop
    while (1)
    {
        // Get a new frame from camera
        const sl_oc::video::Frame frame = cap.getLastFrame();

        // ----> If the frame is valid we can convert, rectify and display it
        if(frame.data!=nullptr && frame.timestamp!=last_ts)
        {
            last_ts = frame.timestamp;

            // ----> Conversion from YUV 4:2:2 to BGR for visualization
            cv::Mat frameYUV = cv::Mat( frame.height, frame.width, CV_8UC2, frame.data );
            cv::cvtColor(frameYUV,frameBGR,cv::COLOR_YUV2BGR_YUYV);
            // <---- Conversion from YUV 4:2:2 to BGR for visualization

            // ----> Extract left and right images from side-by-side
            left_raw = frameBGR(cv::Rect(0, 0, frameBGR.cols / 2, frameBGR.rows));
            right_raw = frameBGR(cv::Rect(frameBGR.cols / 2, 0, frameBGR.cols / 2, frameBGR.rows));
            
            // Display images
            // sl_oc::tools::showImage("left RAW", left_raw, params.res);
            // sl_oc::tools::showImage("right RAW", right_raw, params.res);
            // <---- Extract left and right images from side-by-side

            // ----> Apply rectification
            cv::remap(left_raw, left_rect, map_left_x, map_left_y, cv::INTER_LINEAR );
            cv::remap(right_raw, right_rect, map_right_x, map_right_y, cv::INTER_LINEAR );

            // ----> Save rectified images
            std::stringstream left_filename, right_filename;
            left_filename << left_dir << "/" << frame.timestamp << ".png";
            right_filename << right_dir << "/" << frame.timestamp << ".png";
            
            cv::imwrite(left_filename.str(), left_rect);
            cv::imwrite(right_filename.str(), right_rect);
            // <---- Save rectified images

            // sl_oc::tools::showImage("right RECT", right_rect, params.res);
            // sl_oc::tools::showImage("left RECT", left_rect, params.res);
        }

        // ----> Keyboard handling
        int key = cv::waitKey( 5 );
        if(key=='q' || key=='Q') // Quit
            break;
        // <---- Keyboard handling
    }

    return EXIT_SUCCESS;
}

