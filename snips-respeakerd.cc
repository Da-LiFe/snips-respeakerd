#include <cstring>
#include <memory>
#include <iostream>
#include <csignal>
#include <respeaker.h>
#include <libevdev.h>
#include <mosquitto.h>
#include <sstream>
#include <vector>
#include <chain_nodes/alsa_collector_node.h>
#include <chain_nodes/vep_aec_beamforming_node.h>
#include <chain_nodes/snowboy_1b_doa_kws_node.h>
#include <chain_nodes/snips_1b_doa_kws_node.h>
#include <chain_nodes/aloop_output_node.h>

#include "json.hpp"
#include "toml.h"

extern "C"
{
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
}

#define TOGGLEHOTWORDONTOPIC "hermes/hotword/toggleOn"
#define TOGGLEHOTWORDOFFTOPIC "hermes/hotword/toggleOff"

std::vector<std::string> hotword_models = {
    "/usr/share/respeaker/snips/model"
};

std::vector<std::string> hotword_ids = {
    "default"
};

using namespace std;
using namespace respeaker;

static bool stop = false;

unique_ptr<ReSpeaker> respeaker_ptr;
string source = "default";
bool enable_agc = false;
int agc_level = 10;
string mic_type;
string siteid = "default";
string mqttHost = "localhost";
int mqttPort = 1883;

std::vector<std::string> stringSplit(const std::string& s, char seperator)
{
   std::vector<std::string> output;

    std::string::size_type prev_pos = 0, pos = 0;

    while((pos = s.find(seperator, pos)) != std::string::npos)
    {
        std::string substring( s.substr(prev_pos, pos-prev_pos) );

        output.push_back(substring);

        prev_pos = ++pos;
    }

    output.push_back(s.substr(prev_pos, pos-prev_pos)); // Last word

    return output;
}

bool caseInSensStringCompareCpp11(std::string & str1, std::string &str2)
{
	return ((str1.size() == str2.size()) && std::equal(str1.begin(), str1.end(), str2.begin(), [](char & c1, char & c2) {
	    return (c1 == c2 || std::toupper(c1) == std::toupper(c2));
    }));
}

void SignalHandler(int signal)
{
    cerr << "Caught signal " << signal << ", terminating..." << endl;
    stop = true;
}

void mqtt_message_callback(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *message)
{
    if(message->payloadlen)
    {
        //We always require a payload.  So if we don't get one we just ignore this message

        //lets parse the payload (and extract any common info that might be needed)
        std::string stringSiteId = "";
        try
        {
            auto parsedPayload = nlohmann::json::parse((const char*)message->payload);

            //Try and resolve the site ID from the payload
            auto siteIdValue = parsedPayload.find("siteId");

            if (siteIdValue != parsedPayload.end())
            {
                stringSiteId = (*siteIdValue);
            }
        }
        catch (...)
        {
        }

        size_t messageTopicLength = strlen(message->topic);

        bool hotwordToggleOnCommand = false;
        bool hotwordToggleOffCommand = false;
        if (strncasecmp(TOGGLEHOTWORDONTOPIC, message->topic, messageTopicLength) == 0)
        {
            hotwordToggleOnCommand = true;
        }
        else if (strncasecmp(TOGGLEHOTWORDOFFTOPIC, message->topic, messageTopicLength) == 0)
        {
            hotwordToggleOffCommand = true;
        }
        else
        {
            //Unknown topic.
            return;
        }

        if (hotwordToggleOnCommand || hotwordToggleOffCommand)
        {
            //Check to make sure this is for the right site id before we react to it.
            if (caseInSensStringCompareCpp11(siteid, stringSiteId))
            {
                //Site ID matches.  Sort out what we are being asked to do.

                //Get the chain state data and sort out if we really need to change our state
                ChainSharedData *chainstateData = respeaker_ptr->GetChainSharedDataPtr();

                if (hotwordToggleOnCommand)
                {
                    printf("Toggle hotword ON for our siteid\n");
                    //We only need to ask the chain to change state if we are passivly listening
                    if (chainstateData && chainstateData->state == ChainState::LISTEN_QUIETLY)
                    {
                        respeaker_ptr->SetChainState(ChainState::WAIT_TRIGGER_QUIETLY);
                    }
                }
                else if (hotwordToggleOffCommand)
                {
                    printf("Toggle hotword OFF for our siteid\n");
                    //We only need to change state if we are waiting for a trigger
                    if (chainstateData && chainstateData->state == ChainState::WAIT_TRIGGER_QUIETLY)
                    {
                        respeaker_ptr->SetChainState(ChainState::LISTEN_QUIETLY);
                    }
                }
            }
        }
    }
    else
    {
        //Since there was no payload we will just ignore this.  We are expecting a specific bit of data.
    }


    if(message->payloadlen) {
		printf("%s %s\n", message->topic, message->payload);
	}else{
		printf("%s (null)\n", message->topic);
	}
	fflush(stdout);
}

void mqtt_connect_callback(struct mosquitto *mosq, void *userdata, int result)
{
	int i;
	if(!result)
    {
        //Subscribve to hotword control messages
		mosquitto_subscribe(mosq, NULL, TOGGLEHOTWORDONTOPIC, 2);
        mosquitto_subscribe(mosq, NULL, TOGGLEHOTWORDOFFTOPIC, 2);
        printf("Subscribed to hotword control topics\n");
	}
    else
    {
		fprintf(stderr, "MQTT Connect failed\n");
	}
}

void mqtt_subscribe_callback(struct mosquitto *mosq, void *userdata, int mid, int qos_count, const int *granted_qos)
{
	int i;

	printf("Subscribed (mid: %d): %d", mid, granted_qos[0]);
	for(i=1; i<qos_count; i++){
		printf(", %d", granted_qos[i]);
	}
	printf("\n");
}

void mqtt_log_callback(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
	/* Pring all log messages regardless of level. */
	printf("%s\n", str);
}


/*==============================================================================
*/

static void help(const char *argv0) {
    cout << "alsa_aloop_test [options]" << endl;
    cout << "A demo application for librespeaker." << endl << endl;
    cout << "  -h, --help                               Show this help" << endl;
    cout << "  -s, --source=SOURCE_NAME                 The alsa source (microphone) to connect to" << endl;
    cout << "  -t, --type=MIC_TYPE                      The MICROPHONE TYPE, support: CIRCULAR_6MIC_7BEAM, LINEAR_6MIC_8BEAM, LINEAR_4MIC_1BEAM, CIRCULAR_4MIC_9BEAM" << endl;
    cout << "  -g, --agc=NEGTIVE INTEGER                The target gain level of output, [-31, 0]" << endl;
    //cout << "      --siteid=SITE_ID                     The Snips site id" << endl;
}

int main(int argc, char *argv[])
{
    // Configures signal handling.
    struct sigaction sig_int_handler;
    sig_int_handler.sa_handler = SignalHandler;
    sigemptyset(&sig_int_handler.sa_mask);
    sig_int_handler.sa_flags = 0;
    sigaction(SIGINT, &sig_int_handler, NULL);
    sigaction(SIGTERM, &sig_int_handler, NULL);

    // parse opts
    int c;
    static const struct option long_options[] = {
        {"help",         0, NULL, 'h'},
        {"source",       1, NULL, 's'},
        {"type",         1, NULL, 't'},
        {"agc",          1, NULL, 'g'},
        //{"siteid",       1, NULL, 1000},
        {NULL,           0, NULL,  0}
    };

    while ((c = getopt_long(argc, argv, "hs:o:k:t:g:w", long_options, NULL)) != -1) {
        switch (c) {
        case 'h' :
            help(argv[0]);
            return 0;
        case 's':
            source = string(optarg);
            break;
        case 't':
            mic_type = string(optarg);
            break;
        case 'g':
            enable_agc = true;
            agc_level = stoi(optarg);
            if ((agc_level > 31) || (agc_level < -31)) agc_level = 31;
            if (agc_level < 0) agc_level = (0-agc_level);
            break;
        /*case 1000:
            siteid = string(optarg);
            break;*/
        default:
            return 0;
        }
    }

    //load the snips config file so we can pull out any of the details in it we need
    std::ifstream ifs("/etc/snips.toml");
    toml::ParseResult pr = toml::parse(ifs);

    if (pr.valid())
    {
        const toml::Value& parsedValues = pr.value;

        const toml::Value* mqttSettingString = parsedValues.find("snips-common.mqtt");
        if (mqttSettingString && mqttSettingString->is<std::string>())
        {
            std::vector<std::string> splitMQTTDetails = stringSplit(mqttSettingString->as<std::string>(), ':');
            if (splitMQTTDetails.size() == 2)
            {
                mqttHost = splitMQTTDetails[0];
                mqttPort = atoi(splitMQTTDetails[1].c_str());
            }
        }

        const toml::Value* audioBindString = parsedValues.find("snips-audio-server.bind");
        if (audioBindString && audioBindString->is<std::string>())
        {
            std::vector<std::string> splitAudioBindOptions = stringSplit(audioBindString->as<std::string>(), '@');
            if (splitAudioBindOptions.size() == 2)
            {
                siteid = splitAudioBindOptions[0];
            }
        }

        const toml::Value* hotwordSensitivityString = parsedValues.find("snips-hotword.sensitivity");
        if (hotwordSensitivityString && hotwordSensitivityString->is<std::string>())
        {
            hotwordSensitivityDefault = hotwordSensitivityString->as<std::string>();
        }

        const toml::Value* hotwordModelEntry = parsedValues.find("snips-hotword.model");
        if (hotwordModelEntry && hotwordModelEntry->is<std::vector<std::string>>())
        {
            hotword_models = hotwordModelEntry->as<std::vector<std::string>>();

        }
        
        //TODO: parse snips-hotword.hotword_id to std::vector<std::string> hotword_ids
    }

    //build up our model string and sensitivities string from toml file
    std::stringstream hotwordModelString("");
    std::stringstream hotwordSensitivitiesString("0.5");
    std::string hotwordSensitivityDefault = "0.5";
    int hotwordIndexCount = 0;

    std::cout << "Using the following hotword models:" << std::endl;
    for (std::vector<std::string>::iterator it = hotword_models.begin(); it != hotword_models.end(); it++)
    {
        if (it != hotword_models.begin())
        {
            hotwordModelString << ",";
            hotwordSensitivitiesString << ",";
        }
        std::string hotwordSensitivity;                
        std::vector<std::string> splitModelEntry = stringSplit(*it, '=');

        if(splitModelEntry.size() == 2) {
            hotwordSensitivity = splitModelEntry[1];                  
        } else if(splitModelEntry.size() == 1) {
            hotwordSensitivity = hotwordSensitivityDefault;
        } else {
            std::cerr << "Unable to parse hotword model entry " << *it << std::endl;
            exit(1);
        }

        hotwordModelString << splitModelEntry[0];
        hotwordSensitivitiesString << hotwordSensitivity;

        std::cout << hotwordIndexCount++ << ": " << splitModelEntry[0] << "=" << hotwordSensitivity << std::endl;
    }


    //Setup libevdev to get the button pushes
    struct libevdev *dev = NULL;
    int fd;
    int rc = 1;

    fd = open("/dev/input/event0", O_RDONLY|O_NONBLOCK);
    rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        fprintf(stderr, "Failed to init libevdev (%s)\n", strerror(-rc));
        exit(1);
    }

    //Startup the MQTT connection
    struct mosquitto *mosq = NULL;
    bool clean_session = true;

    printf("Connecting to MQTT server: %s:%d\n", mqttHost.c_str(), mqttPort);
    printf("Using siteid: %s\n", siteid.c_str());

    mosquitto_lib_init();
    mosq = mosquitto_new(NULL, clean_session, NULL);

    if(!mosq){
        fprintf(stderr, "Error: Unable to allocate the MQTT connection - Out of memory.\n");
        return 1;
    }

    //mosquitto_log_callback_set(mosq, mqtt_log_callback);
    mosquitto_connect_callback_set(mosq, mqtt_connect_callback);
    mosquitto_message_callback_set(mosq, mqtt_message_callback);
    //mosquitto_subscribe_callback_set(mosq, mqtt_subscribe_callback);

    if(mosquitto_connect(mosq, mqttHost.c_str(), mqttPort, 60))
    {
		fprintf(stderr, "Error: Unable to connect to the MQTT server.\n");
		return 1;
	}
    mosquitto_loop_start(mosq);

    std::stringstream audioFrameTopic;
    audioFrameTopic << "hermes/audioServer/" << siteid << "/audioFrame";

    unique_ptr<AlsaCollectorNode> collector;
    unique_ptr<VepAecBeamformingNode> vep_beam;
    //unique_ptr<Snowboy1bDoaKwsNode> kws;
    unique_ptr<Snips1bDoaKwsNode> kws;
    unique_ptr<AloopOutputNode> aloop;

    collector.reset(AlsaCollectorNode::Create(source, 48000, 8, false));
    vep_beam.reset(VepAecBeamformingNode::Create(MicType::CIRCULAR_6MIC_7BEAM, true, 6, false));
    aloop.reset(AloopOutputNode::Create("hw:Loopback,0,0",true));

    //TODO Support for snips and snowboy models
    /*kws.reset(Snowboy1bDoaKwsNode::Create("/home/pi/snowboy/resources/common.res", 
			"/home/pi/snowboy/resources/models/computer_snowboy.pmdl",
			"0.4",
			5, enable_agc, true));*/
    kws.reset(Snips1bDoaKwsNode::Create(hotwordModelString.str(), std::stof(hotwordSensitivitiesString.str()), enable_agc, true));

    //Snips deals with notification of the transfer state with its own messages
    //So we can disable the auto transfer states here
    kws->DisableAutoStateTransfer();
    kws->SetDoAecWhenListen(true);

    if (enable_agc)
    {
        kws->SetAgcTargetLevelDbfs(agc_level);
        cout << "AGC = -"<< agc_level<< endl;
    }
    else {
        cout << "Disable AGC" << endl;
    }

    vep_beam->Uplink(collector.get());
    kws->Uplink(vep_beam.get());
    aloop->Uplink(kws.get());

    respeaker_ptr.reset(ReSpeaker::Create(TRACE_LOG_LEVE));
    respeaker_ptr->RegisterChainByHead(collector.get());
    respeaker_ptr->RegisterOutputNode(aloop.get());
    respeaker_ptr->RegisterDirectionManagerNode(kws.get());
    respeaker_ptr->RegisterHotwordDetectionNode(kws.get());

    if (!respeaker_ptr->Start(&stop))
    {
        cout << "Can not start the respeaker node chain." << endl;
        return -1;
    }

    // You should call this after respeaker->Start()
    aloop->SetMaxBlockDelayTime(250);

    size_t num_channels = respeaker_ptr->GetNumOutputChannels();
    int rate = respeaker_ptr->GetNumOutputRate();
    cout << "num channels: " << num_channels << ", rate: " << rate << endl;

    int tick;
    int hotword_index = 0, hotword_trigger_count = 0;
    int dir = 0;
    bool activationTriggered = false;

    bool physicalButtonTrigger = false;
    uint16_t previousPhysicalButtonValue = 0;

    while (!stop)
    {
        ChainSharedData *chainstateData = respeaker_ptr->GetChainSharedDataPtr();

        //Do the physical button checking
        {
            physicalButtonTrigger = false;

            int eveventcount = 0;
            do
            {
                struct input_event ev;
                rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                eveventcount++;

                if (rc == 0)
                {
                    if (ev.type == EV_KEY && ev.code == KEY_F24)
                    {
                        if (previousPhysicalButtonValue != ev.value && ev.value == 0)
                        {
                            physicalButtonTrigger = true;
                        }
                        previousPhysicalButtonValue = ev.value;

                        printf("Event: %s %s %d\n",
                                libevdev_event_type_get_name(ev.type),
                                libevdev_event_code_get_name(ev.type, ev.code),
                                ev.value);
                    }
                }
            } while (rc == 0 && eveventcount < 20);
        }

        if (chainstateData && (chainstateData->state == ChainState::WAIT_TRIGGER_QUIETLY || chainstateData->state == ChainState::WAIT_TRIGGER_WITH_BGM))
        {
            //Check to see if we have any hotword triggered
            hotword_index = respeaker_ptr->DetectHotword();

            if (hotword_index > 0 || physicalButtonTrigger)
            {
                bool bailoutAndIgnore = false;
                std::string hotwordId = "default";
                //Hotword has been detected (or the physical button was pushed).  Time to notify snips
                if (physicalButtonTrigger)
                {
                    //if we are triggered by a physical button there is no direction.  Just hardcode to 0
                    dir = 0;
                    hotwordId = "physicalbutton";

                    //since we can have multiple button pushes we want to ignore any after the first one
                    //we do that by using the chain state (so we also as a side effect ignore button pushes after a hotword activation)
                    //Check to see if we are in state LISTEN_QUIETLY
                    if (chainstateData && (chainstateData->state == ChainState::LISTEN_QUIETLY || chainstateData->state == ChainState::LISTEN_WITH_BGM))
                    {
                        //We have already had a hotword (or physical button) trigger.  So just bail out
                        bailoutAndIgnore = true;
                    }
                }
                else
                {
                    //When trigged by the hotword engine we can fetch the DOA
                    dir = respeaker_ptr->GetDirection();

                    //Sort out the wakeword that was used and reflect that.
                    int activatedHotwordIndex = hotword_index - 1;
                    if (activatedHotwordIndex < hotword_ids.size())
                    {
                        hotwordId = hotword_ids.at(activatedHotwordIndex);
                    }
                    else
                    {
                        hotwordId = "unknownhotword";
                    }
                }

                if (!bailoutAndIgnore)
                {
                    //We build this up manually since snips apparenly needs the json in a specific order to function
                    std::stringstream payload;
                    payload << "{"
                        << "\"siteId\":\"" << siteid << "\","
                        << "\"modelId\":\"" << hotwordId << "\","
                        << "\"modelVersion\":\"1\","
                        << "\"modelType\":" << "\"universal\"" << ","
                        << "\"currentSensitivity\":" << 0.5 << ","
                        << "\"direction\":" << dir
                        << "}";

                    std::stringstream topic;
                    topic << "hermes/hotword/" << hotwordId << "/detected";

                    //push the hotword activation message to MQTT
                    mosquitto_publish(mosq, NULL, topic.str().c_str(), payload.str().length(), payload.str().c_str(), 2, false);

                    hotword_trigger_count++;
                    cout << "hotword: " << hotwordId << ", direction: " << dir << ", hotword_count = " << hotword_trigger_count << endl;

                    //Switch the chain to the listen mode
                    respeaker_ptr->SetChainState(ChainState::LISTEN_QUIETLY);
                }
		
	    }
        }
	if (chainstateData && (chainstateData->state != ChainState::LISTEN_QUIETLY) && tick++ % 100 == 0)
	{
	    std::cout << "collector: " << collector->GetQueueDeepth() << ", vep_beam: " << vep_beam->GetQueueDeepth() << ", kws: " << kws->GetQueueDeepth() << std::endl;
	}
 
    }

    cout << "stopping the respeaker worker thread..." << endl;
    respeaker_ptr->Pause();
    respeaker_ptr->Stop();

    //Clean up the libevdev descriptor
    close(fd);

    //Cleanup the mqtt connection
    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosq = NULL;
    mosquitto_lib_cleanup();

    cout << "cleanup done." << endl;
    return 0;
}
