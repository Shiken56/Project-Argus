#ifndef NNLIB_H
#define NNLIB_H

#include <stdint.h>
#include <stdbool.h>
#include "stai.h"

/* =========================================================================
 * Function pointer vtable — one per model.
 * Each ST Edge AI generated model exposes identically-shaped functions
 * (init, run, get_inputs, get_outputs). The vtable lets nnlib call them
 * generically so both OD and FX (ReID) models share the same API.
 * ========================================================================= */
typedef struct {
    stai_return_code (*init)       (stai_network *net);
    stai_return_code (*run)        (stai_network *net, stai_run_mode mode);
    stai_return_code (*get_inputs) (stai_network *net, stai_ptr *in,  stai_size *n);
    stai_return_code (*get_outputs)(stai_network *net, stai_ptr *out, stai_size *n);
} nnlib_vtable_t;

/* Structure to hold NN initialization configurations */
typedef struct {
    stai_network    *network;
    void            *external_weights_addr; /* NOR Flash address where model weights are stored */
    nnlib_vtable_t   vtable;
} nnlib_config_t;

/* High-level NN Library API */

/**
 * @brief Initialize the shared hardware: NPU clocks, AXISRAM, NOR flash, NPU IRQ.
 *        Must be called exactly ONCE before any nnlib_init() call.
 */
void nnlib_hardware_init(void);

/**
 * @brief Initialize a specific AI model (calls vtable.init internally).
 *        Does NOT re-initialize hardware — nnlib_hardware_init() must be called first.
 * @param config Configuration containing the network instance, weight address, and vtable
 * @return true if successful, false otherwise
 */
bool nnlib_init(nnlib_config_t *config);

/**
 * @brief Set the input buffer for the model
 * @param config Configuration containing the network instance
 * @param input_data Pointer to the input data (e.g., image buffer)
 * @param size Size of the input data
 * @return true if successful, false otherwise
 */
bool nnlib_set_input(nnlib_config_t *config, void *input_data, uint32_t size);

/**
 * @brief Run inference on the model synchronously
 * @param config Configuration containing the network instance
 * @param inference_time_ms Pointer to store the execution time in milliseconds
 * @return true if successful, false otherwise
 */
bool nnlib_run_inference(nnlib_config_t *config, uint32_t *inference_time_ms);

/**
 * @brief Get the output buffer from the model
 * @param config Configuration containing the network instance
 * @param output_data Pointer to a pointer that will hold the output buffer address
 * @return true if successful, false otherwise
 */
bool nnlib_get_output(nnlib_config_t *config, void **output_data);

#endif /* NNLIB_H */
