#include "edgegateway.h"
#include "edgeutil.h"
#include <unistd.h>
#include <iostream>

using namespace std;

void transmit_heartbeat_to_datonis(struct thing *t) {
    int response = transmit_thing_heartbeat(t);
    if (response != ERR_OK) {
        fprintf(stderr, "Failed to send thing heartbeat. Response Code: %d, Error: %s\n", response, get_error_code_message(response));
    } else {
        cout << "Heartbeat Response: " << response << std::endl;
    }
}

static void execute_instruction(char *thing_key, char *alert_key, char *instruction) {
    cout << "\nExecute Instruction: " << instruction << std::endl;
    transmit_instruction_feedback(alert_key, 1, "A demo warning to show feedback", "{\"some_key\":\"some_value\"}");
}

static void send_example_alert(struct thing *t, int level, char * msg) {
    int response = transmit_thing_alert(t, level, msg, "{\"foo\":\"bar\"}");
    if (response != ERR_OK) {
        fprintf(stderr, "Failed to send alert to datonis. Response code: %d, Error: %s\n", response, get_error_code_message(response));
    } else {
        printf("Successfully sent an alert to datonis with level: %d, message: %s\n", level, msg);
    }
}


static void send_example_alerts(struct thing *t) {
    send_example_alert(t, 0, "Example INFO alert from C Agent");
    send_example_alert(t, 1, "Example WARNING alert from C Agent");
    send_example_alert(t, 2, "Example ERROR alert from C Agent");
    send_example_alert(t, 3, "Example CRITICAL alert from C Agent");
}

int main() {
    struct thing t;
    char buf[500];
    char waypoint[50];
    string access_key = "93a768t53fd3e21tf7cc357c9b3f2f446297d872";
    string secret_key = "cefdf5e449c93e32567155f6ddct9d47e38tfd57";
    initialize(&access_key[0], &secret_key[0]);
    //create_thing(struct thing *thing, char* key, char* name, char* description, instruction_handler handler)
    create_thing(&t, "742923439d", "Compressor 1", "Thing for compressors", execute_instruction);
    int response = 0;
    response = connect_datonis();
    if (response != ERR_OK) {
        fprintf(stderr, "Failed to connect to Datonis!\n");
        exit(1);
    }

    // Thing registration is optional to send event.
    /*response = register_thing(&t);
    if (response != ERR_OK) {
        fprintf(stderr, "Failed to Register Thing. Response Code: %d, Error: %s\n", response, get_error_code_message(response));
        exit(1);
    } else {
        printf("Successfully Registered thing with Datonis!\n");
    }*/

    if (1) {
        send_example_alerts(&t);
    }

    int counter = 5;
    while(1) {

        if (counter == 5) {
            transmit_heartbeat_to_datonis(&t);
            counter = 0;
        }
        counter++;

        yield(2000);
        yield(2000);

    sprintf(buf, "{\"pressure\":%ld,\"temperature\":%ld}", random(), random());
	//waypoint format: [latitude, longitude], where latitude and longitude must be double values.	
	sprintf(waypoint, "[19.%ld,73.%ld]", random() % 100000, random() % 100000);

        /* You can send data to Datonis in a compressed form.
         * This not only reduces the network bandwidth usage for your agent
         * But also improves network latency to the Datonis server
         * This happens at the cost of some extra processing power needed on the device
         * You can still chose to send data in a uncompressed form
         */
	//You can send both meta-data as well as waypoint in single request.
	//Pass NULL in place of buf or waypoint if you don't want to send that data.
	//Atleast one from buf or waypoint should be passed. Both cannot be NULL.
	response = transmit_compressed_thing_data(&t, buf, NULL);
	//response = transmit_compressed_thing_data(&t, NULL, waypoint);
	//response = transmit_compressed_thing_data(&t, buf, waypoint);
	
        /* Uncomment to send data in an uncompressed way */
        // response = transmit_thing_data(&t, buf, waypoint);

        if (response != ERR_OK) {
            fprintf(stderr, "Failed to send thing data. Response Code: %d, Error: %s\n", response, get_error_code_message(response));
        } else {
            printf("Transmitt Data Response: %d\n", response);
        }
        sleep(5);
    }
    return 0;
}