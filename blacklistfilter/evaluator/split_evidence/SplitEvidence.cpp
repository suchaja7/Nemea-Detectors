#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <unirec/unirec.h>
#include <libtrap/trap.h>
#include <unirec/unirec.h>
#include <unirec/unirec2csv.h>
#include "fields.h"

#include "FileHandler.h"

trap_module_info_t *module_info = NULL;

/**
 * Definition of basic module information - module name, module description, number of input and output interfaces
 */
#define MODULE_BASIC_INFO(BASIC) \
  BASIC("split_evidence", \
        "The module saves flows into files with the name given by value of UniRec field." \
        "", 1, 0)

/**
 * Definition of module parameters - every parameter has short_opt, long_opt, description,
 * flag whether an argument is required or it is optional and argument type which is NULL
 * in case the parameter does not need argument.
 * Module parameter argument types: int8, int16, int32, int64, uint8, uint16, uint32, uint64, float, string
 */
#define MODULE_PARAMS(PARAM) \
  PARAM('f', "field", "UniRec field used for splitting.", required_argument, "string") \
  PARAM('p', "path", "Path to directory where to store files.", required_argument, "string")
//PARAM(char, char *, char *, no_argument  or  required_argument, char *)

#define CHECK_INTERVAL 5000
#define CLOSE_TIMEOUT 10000

#define MAX_KEYSTR_SIZE 400
static const char KEYSTR_DELIM[] = ",";
static int stop = 0;

TRAP_DEFAULT_SIGNAL_HANDLER(stop = 1)
urcsv_t *csv = NULL;

ur_template_t *in_tmplt = NULL;

int main(int argc, char **argv) {
    int ret;
    signed char opt;
    const char *field_name = NULL;
    const char *output_path = NULL;

    /* **** TRAP initialization **** */

    INIT_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)

    /**
     * Let TRAP library parse program arguments, extract its parameters and initialize module interfaces
     */
    TRAP_DEFAULT_INITIALIZATION(argc, argv, *module_info);

    /**
     * Register signal handler.
     */
    TRAP_REGISTER_DEFAULT_SIGNAL_HANDLER();

    /**
     * Parse program arguments defined by MODULE_PARAMS macro with getopt() function (getopt_long() if available)
     * This macro is defined in config.h file generated by configure script
     */
    while ((opt = TRAP_GETOPT(argc, argv, module_getopt_string, long_options)) != -1) {
        switch (opt) {
            case 'f':
                field_name = optarg;
                break;
            case 'p':
                output_path = optarg;
                break;
            default:
                fprintf(stderr, "Invalid arguments.\n");
                FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
                TRAP_DEFAULT_FINALIZATION();
                return -1;
        }
    }

    if (field_name == NULL || output_path == NULL) {
        fprintf(stderr, "Error: missing required parameter -f or -p.\n");

        // Do all necessary cleanup in libtrap before exiting
        TRAP_DEFAULT_FINALIZATION();

        // Release allocated memory for module_info structure
        FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)

        // Free unirec template
        ur_free_template(in_tmplt);
        ur_finalize();

        return 1;
    }

    /* **** Create UniRec templates **** */

    trap_set_required_fmt(0, TRAP_FMT_UNIREC, "");

    /* **** Main processing loop **** */

    int field_id = UR_E_INVALID_NAME;

    FileHandler file_handler(output_path, std::chrono::milliseconds(CHECK_INTERVAL), std::chrono::milliseconds(CLOSE_TIMEOUT));
    file_handler.start_handler();
    // Read data from input, process them and write to output
    while (!stop) {
        const void *in_rec;
        uint16_t in_rec_size;

        // Receive data from input interface 0.
        // Block if data are not available immediately (unless a timeout is set using trap_ifcctl)
        ret = TRAP_RECEIVE(0, in_rec, in_rec_size, in_tmplt);
        if (ret == TRAP_E_FORMAT_CHANGED) {
            free(csv);
            csv = urcsv_init(in_tmplt, ',');
            if (csv == NULL) {
                fprintf(stderr, "Failed to initialize UniRec2CSV converter.\n");
                break;
            }
        }
        // Handle possible errors
        TRAP_DEFAULT_RECV_ERROR_HANDLING(ret, continue, break);

        if (field_id == UR_E_INVALID_NAME) {
            /* first initialization of splitter key */
            field_id = ur_get_id_by_name(field_name);
            if (field_id == UR_E_INVALID_NAME) {
                fprintf(stderr, "Error: field %s was not found in the input template.\n", field_name);
                break;
            }
        }

        // Check size of received data
        if (in_rec_size < ur_rec_fixlen_size(in_tmplt)) {
            if (in_rec_size <= 1) {
                break; // End of data (used for testing purposes)
            } else {
                fprintf(stderr, "Error: data with wrong size received (expected size: >= %hu, received size: %hu)\n",
                        ur_rec_fixlen_size(in_tmplt), in_rec_size);
                break;
            }
        }

        char keystr[MAX_KEYSTR_SIZE];

        uint32_t keysize = urcsv_field(keystr, MAX_KEYSTR_SIZE - 1, in_rec, static_cast<ur_field_type_t>(field_id), in_tmplt);
        keystr[keysize] = 0;

        char *r;
        while ((r = strchr(keystr, '/')) != NULL) {
            *r = '_';
        }

        if ( keysize == 0 ) {
            strcpy(keystr, "UNKNOWN");
            keysize = strlen("UNKNOWN");
        }

        char idstr[MAX_KEYSTR_SIZE];
        char *keystr_ptr = keystr;

        // Remove quotes from the key string
        if (keystr[0] == '"') {
            keystr_ptr++;
            keystr[keysize - 1] = 0;
            keysize -= 2;
        }

        strcpy(idstr, keystr_ptr);
        char *token = strtok(idstr, KEYSTR_DELIM);

        // Handle multiple IDs inside the 'field_id'
        if (token == NULL) {
            file_handler.write_to_file(keystr_ptr, in_rec, csv);
        } else {
            file_handler.write_to_file(token, in_rec, csv);

            while ( (token = strtok(NULL, ",")) != NULL )
                file_handler.write_to_file(token, in_rec, csv);
        }
    }
    
    // Do all necessary cleanup in libtrap before exiting
    TRAP_DEFAULT_FINALIZATION();

    // Release allocated memory for module_info structure
    FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS)

    // Free unirec template
    urcsv_free(&csv);
    ur_free_template(in_tmplt);
    ur_finalize();

    return 0;
}

