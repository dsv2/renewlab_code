/*
 Copyright (c) 2018-2020, Rice University 
 RENEW OPEN SOURCE LICENSE: http://renew-wireless.org/license
 Author(s): Rahman Doost-Mohamamdy: doost@rice.edu
 
---------------------------------------------------------------------
 main function
 - initializes all Clients
 - Brings up Recorder and the BaseStation
---------------------------------------------------------------------
*/

#include "include/data_generator.h"
#include "include/recorder.h"
#include "include/signalHandler.hpp"
#include <gflags/gflags.h>

DEFINE_bool(gen_ul_bits, false,
    "Generate random bits for uplink transmissions, otherwise read from file!");
DEFINE_string(conf, "files/conf.json", "JSON configuration file name");
DEFINE_string(storepath, "logs", "Dataset store path");

int main(int argc, char* argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    Config config(FLAGS_conf, FLAGS_storepath);
    int ret = EXIT_SUCCESS;
    if (FLAGS_gen_ul_bits) {
        DataGenerator dg(&config);
        dg.GenerateData(FLAGS_storepath);
    } else {
        try {
            SignalHandler signalHandler;

            // Register signal handler to handle kill signal
            signalHandler.setupSignalHandlers();
            config.loadULData(FLAGS_storepath);
            Sounder::Recorder dr(&config);
            dr.do_it();
            ret = EXIT_SUCCESS;
        } catch (SignalException& e) {
            std::cerr << "SignalException: " << e.what() << std::endl;
            ret = EXIT_FAILURE;
        } catch (const std::exception& exc) {
            std::cerr << "Program terminated Exception: " << exc.what()
                      << std::endl;
            ret = EXIT_FAILURE;
        }
    }
    return ret;
}
