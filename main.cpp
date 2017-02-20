/*
    This file is part of WARG's computer-vision

    Copyright (c) 2015, Waterloo Aerial Robotics Group (WARG)
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.
    3. Usage of this code MUST be explicitly referenced to WARG and this code
       cannot be used in any competition against WARG.
    4. Neither the name of the WARG nor the names of its contributors may be used
       to endorse or promote products derived from this software without specific
       prior written permission.

    THIS SOFTWARE IS PROVIDED BY WARG ''AS IS'' AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL WARG BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <boost/lexical_cast.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread.hpp>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/algorithm/string.hpp>
#include <functional>
#include <boost/program_options.hpp>
#include <queue>
#include <chrono>
#include <iostream>
#include <fstream>
#include "frame.h"
#include "target_identifier.h"
#include "imgimport.h"
#include "decklink_import.h"
#include "pictureimport.h"
#include "metadata_input.h"
#include "target.h"

using namespace std;
using namespace boost;
using namespace cv;
namespace logging = boost::log;
namespace po = boost::program_options;

const int BUFFER_SIZE = 20;

Frame* next_image();
int handle_args(int argc, char** argv);
void handle_input();
queue<Frame*> in_buffer;
queue<Target*> out_buffer;
queue<Frame*> intermediate_buffer;

boost::asio::io_service ioService;
boost::thread_group threadpool;

vector<string> file_names;
int workers = 0;
bool readingFrames = false;
string outputDir = "./";
bool intermediate = false;
int processors;

// Processing module classes
ImageImport * importer = NULL;
TargetIdentifier identifier;
MetadataInput * logReader = NULL;

double aveFrameTime = 1000;
int frameCount = 0;

void worker(Frame* f) {
    auto start = std::chrono::steady_clock::now();
    workers++;
    assert(!f->get_img().empty());
    identifier.process_frame(f);
    if (intermediate && f->get_objects().size() > 0) {
        intermediate_buffer.push(f);
    }

    workers--;
    auto end = std::chrono::steady_clock::now();
    auto diff = end - start;
    aveFrameTime = (std::chrono::duration <double, std::milli>(diff).count() + aveFrameTime*frameCount)/++frameCount;
}

void read_images() {
    Frame* currentFrame;
    while (readingFrames) {
        if (in_buffer.size() < BUFFER_SIZE) {
            Frame* f = importer->next_frame();
            if (f) {
                in_buffer.push(f);
            }
            else {
                readingFrames = false;
            }
        }
        boost::this_thread::sleep(boost::posix_time::milliseconds(aveFrameTime/processors));
    }
}

void assign_workers() {
    Frame* current;
    while (readingFrames || in_buffer.size() > 0) {
        if (in_buffer.size() > 0) {
            current = in_buffer.front();
            // spawn worker to process image;
            BOOST_LOG_TRIVIAL(trace) << "Spawning worker...";
            ioService.post(boost::bind(worker, current));
            in_buffer.pop();
        }

        boost::this_thread::sleep(boost::posix_time::milliseconds(30));
    }
}

void output() {
    filebuf fb;
    while (readingFrames || out_buffer.size() > 0 || intermediate_buffer.size() > 0 || workers > 0) {
        if (out_buffer.size() > 0) {
            fb.open("out.txt", ios::app);
            ostream out(&fb);

            out << out_buffer.front();
            // Output
            out_buffer.pop();
            fb.close();
        }
        if (intermediate_buffer.size() > 0) {
            Frame * f = intermediate_buffer.front();
            f->save(outputDir);
            intermediate_buffer.pop();
        }
        boost::this_thread::sleep(boost::posix_time::milliseconds(30));
    }
    ioService.stop();
}

void init() {
#ifdef RELEASE
    logging::core::get()->set_filter(logging::trivial::severity >= logging::trivial::error);
#else
    logging::core::get()->set_filter(logging::trivial::severity >= logging::trivial::debug);
#endif
}

int main(int argc, char** argv) {
    init();
    int retArg;
    if (retArg = handle_args(argc, argv) != 0)
        return retArg;

    boost::asio::io_service::work work(ioService);
    processors = boost::thread::hardware_concurrency();

    while (!cin.eof()) handle_input();

    threadpool.join_all();
    delete logReader;
    delete importer;
    return 0;
}

void queue_work(std::function<void()> func) {
    ioService.post(boost::bind(func));
    while(threadpool.size() < processors)
        threadpool.create_thread(boost::bind(&boost::asio::io_service::run, &ioService));
}

/**
 * @class Command
 * @brief Describes a CLI Command that can be run in the program's repl
 */
class Command {
public:
    /**
     * @brief Constructor for a CLI command
     * @arg name Command name
     * @arg desc Command Description
     * @arg args List of argument names
     * @arg execute Function to execute when command is run, The vector of strings contains the arguments passed to the command at runtime
     */
    Command(string name, string desc, initializer_list<string> args, std::function<void(vector<string>)> execute): name(name), desc(desc), args(args), execute(execute) { }
    string name, desc;
    vector<string> args;
    std::function<void(vector<string>)> execute;
};

vector<Command> commands = {
    Command("help", "display this help message", {}, [=](vector<string> args) {
        cout << "Commands:" << endl << endl;
        string space = string("");
        for (Command cmd : commands) {
            space.resize(20 - cmd.name.length() + 1 + boost::algorithm::join(cmd.args, " ").length(), ' ');
            cout << cmd.name << " " << boost::algorithm::join(cmd.args, " ") << space << " - " << cmd.desc << endl;
        }
    }),
    Command("log.info", "sets log level to info", {}, [](vector<string> args) {
        logging::core::get()->set_filter(logging::trivial::severity >= logging::trivial::info);
    }),
    Command("log.debug", "sets log level to debug", {}, [](vector<string> args) {
        logging::core::get()->set_filter(logging::trivial::severity >= logging::trivial::debug);
    }),
    Command("log.error", "sets log level to error", {}, [](vector<string> args) {
        logging::core::get()->set_filter(logging::trivial::severity >= logging::trivial::error);
    }),
    Command("frames.start", "starts fetching frames", {}, [](vector<string> args) {
        if (!readingFrames) {
            queue_work(read_images);
            readingFrames = true;
        } else {
           BOOST_LOG_TRIVIAL(error) << "Frames are already being fetched";
        }
    }),
    Command("frames.stop", "starts fetching frames", {}, [](vector<string> args) {
        if (readingFrames) {
            readingFrames = false;
        } else {
           BOOST_LOG_TRIVIAL(error) << "Frames are not being fetched";
        }
    })
};

void handle_input() {
    cout << "wargcv$ ";
    string input;
    getline(cin, input);
    vector<string> args;
    boost::split(args, input, boost::is_any_of(" "));
    bool valid = false;

    for (Command cmd : commands) {
        if (args.size() > 0 && args[0].compare(cmd.name) == 0) {
            if (args.size() - 1 == cmd.args.size()) {
                BOOST_LOG_TRIVIAL(info) << "Executing command: " << cmd.name;
                cmd.execute(vector<string>(args.begin() + 1, args.end()));
                valid = true;
            } else {
                cout << "Usage: " << endl;
                cout << cmd.name << " " << boost::algorithm::join(cmd.args, " ") << " - " << cmd.desc << endl;
            }
        }
    }
    if (!valid && !cin.eof()) {
        BOOST_LOG_TRIVIAL(info) << "Executing command: " << commands[0].name;
        commands[0].execute(vector<string>());
    }
}

int handle_args(int argc, char** argv) {
    try {
        po::options_description description("Usage: warg-cv [OPTION]");

        description.add_options()("help,h", "Display this help message")
            ("images,i", po::value<string>(), "Directory containing image files to be processed")
            ("video,v", po::value<int>(), "Video device to capture images from")
#ifdef HAS_DECKLINK
            ("decklink,d", "Use this option to capture video from a connected Decklink card")
#endif // HAS_DECKLINK
            ("telemetry,t", po::value<string>(), "Path of the telemetry log for the given image source")
            ("addr,a", po::value<string>(), "Address to connect to to recieve telemetry log")
            ("port,p", po::value<string>(), "Port to connect to to recieve telemetry log")
            ("output,o", po::value<string>(), "Directory to store output files; default is current directory")
            ("intermediate", "When this is enabled, program will output intermediary frames that contain objects of interest");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, description), vm);
        po::notify(vm);

        if (vm.count("help") || vm.size() == 0) {
            cout << description << endl;
            return 1;
        }

        int devices = vm.count("video") + vm.count("decklink") + vm.count("images");
        if (devices > 1) {
            cout << "Invalid options: You can only specify one image source at a time" << endl;
            return 1;
        } else if(devices == 0) {
            cout << "Error: You must specify an image source!" << endl;
            return 1;
        }
        if (!vm.count("telemetry") && !(vm.count("addr") && vm.count("port"))) {
            cout << "Invalid options; You must specify a telemetry file, or port and address" << endl;
            return 1;
        }

        if (vm.count("telemetry")) {
            logReader = new MetadataInput(vm["telemetry"].as<string>());
        } else if (vm.count("addr") && vm.count("port")) {
            logReader = new MetadataInput(vm["addr"].as<string>(), vm["port"].as<string>());
        }

#ifdef HAS_DECKLINK
        if (vm.count("decklink")) {
            importer = new DeckLinkImport(logReader);
        }
#endif // HAS_DECKLINK

        if (vm.count("images")) {
            string path = vm["images"].as<string>();
            importer = new PictureImport(path, logReader);
        }

        if (vm.count("output")) {
            outputDir = vm["output"].as<string>();
        }

        if (vm.count("intermediate")) {
            intermediate = true;
        }
    }
    catch (std::exception& e) {
        cout << e.what() << endl;
        return 1;
    }

    return 0;
}
