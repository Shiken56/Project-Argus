#ifndef NNLIB_H
#define NNLIB_H

#include <stdint.h>
#include <stdbool.h>
#include "stai_od_model.h"

/* Structure to hold NN initialization configurations */
typedef struct {
    stai_network* network;
    void* external_weights_addr; // If using relocatable weights
} nnlib_config_t;

/* High-level NN Library API */

/**
 * @brief Initialize the Neural Network hardware and model
 * @param config Configuration containing the network instance and optional weight address
 * @return true if successful, false otherwise
 */
bool nnlib_init(nnlib_config_t* config);

/**
 * @brief Set the input buffer for the model
 * @param config Configuration containing the network instance
 * @param input_data Pointer to the input data (e.g., image buffer)
 * @param size Size of the input data
 * @return true if successful, false otherwise
 */
bool nnlib_set_input(nnlib_config_t* config, void* input_data, uint32_t size);

/**
 * @brief Run inference on the model synchronously
 * @param config Configuration containing the network instance
 * @param inference_time_ms Pointer to store the execution time in milliseconds
 * @return true if successful, false otherwise
 */
bool nnlib_run_inference(nnlib_config_t* config, uint32_t* inference_time_ms);

/**
 * @brief Get the output buffer from the model
 * @param config Configuration containing the network instance
 * @param output_data Pointer to a pointer that will hold the output buffer address
 * @return true if successful, false otherwise
 */
bool nnlib_get_output(nnlib_config_t* config, void** output_data);

#endif /* NNLIB_H */
