#include "nnlib.h"
#include "stai.h"
#include "stm32n6570_discovery_xspi.h"
#include "stm32n6xx_hal.h"
#include <string.h>
#include <tm/tmonitor.h>

#define PRINT(fmt, ...) tm_printf((const UB *)(fmt), ##__VA_ARGS__)

/* Track whether hardware has been initialized (call only once) */
static bool s_hw_initialized = false;

/* Track whether STAI runtime has been initialized (call only once) */
static bool s_runtime_initialized = false;

void nnlib_hardware_init(void) {
  if (s_hw_initialized) return;
  s_hw_initialized = true;

  /* 1. NPU Reset and Clock Enable */
  __HAL_RCC_CACHEAXI_CLK_ENABLE();
  __HAL_RCC_NPU_CLK_ENABLE();
  __HAL_RCC_NPU_FORCE_RESET();
  __HAL_RCC_NPU_RELEASE_RESET();

  /* 1b. Initialize and Enable CACHEAXI */
  CACHEAXI_HandleTypeDef hcacheaxi = {0};
  hcacheaxi.Instance = CACHEAXI;
  HAL_CACHEAXI_Init(&hcacheaxi);
  while (HAL_CACHEAXI_Enable(&hcacheaxi) == HAL_BUSY)
    ;

  /* 2. Enable NPU RAMs (AXISRAM3, AXISRAM4, AXISRAM5, AXISRAM6) */
  __HAL_RCC_AXISRAM3_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM4_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM5_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM6_MEM_CLK_ENABLE();
  __HAL_RCC_RAMCFG_CLK_ENABLE();

  RAMCFG_HandleTypeDef hramcfg = {0};

  hramcfg.Instance = RAMCFG_SRAM3_AXI;
  HAL_RAMCFG_EnableAXISRAM(&hramcfg);

  hramcfg.Instance = RAMCFG_SRAM4_AXI;
  HAL_RAMCFG_EnableAXISRAM(&hramcfg);

  hramcfg.Instance = RAMCFG_SRAM5_AXI;
  HAL_RAMCFG_EnableAXISRAM(&hramcfg);

  hramcfg.Instance = RAMCFG_SRAM6_AXI;
  HAL_RAMCFG_EnableAXISRAM(&hramcfg);

  /* 3. Sleep Mode Clock Configurations (for WFE) */
  __HAL_RCC_NPU_CLK_SLEEP_ENABLE();
  __HAL_RCC_CACHEAXI_CLK_SLEEP_ENABLE();
  __HAL_RCC_FLEXRAM_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM1_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM2_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM3_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM4_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM5_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM6_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_XSPI2_CLK_SLEEP_ENABLE();

  /* 4. Initialize External OSPI Flash (XSPI2) mapped at 0x71000000 for ML
   * Weights */
  BSP_XSPI_NOR_Init_t NOR_Init;
  NOR_Init.InterfaceMode = BSP_XSPI_NOR_OPI_MODE;
  NOR_Init.TransferRate = BSP_XSPI_NOR_DTR_TRANSFER;
  if (BSP_XSPI_NOR_Init(0, &NOR_Init) != BSP_ERROR_NONE) {
    PRINT("[NNLIB ERROR] Failed to init NOR Flash (XSPI2)\r\n");
  }

  if (BSP_XSPI_NOR_EnableMemoryMappedMode(0) != BSP_ERROR_NONE) {
    PRINT(
        "[NNLIB ERROR] Failed to enable Memory Mapped Mode for NOR Flash\r\n");
  }

  /* 5. Set NPU IRQ Priority */
  HAL_NVIC_SetPriority(NPU0_IRQn, 5, 0);
}

bool nnlib_init(nnlib_config_t *config) {
  if (!config || !config->network) {
    return false;
  }

  /* Initialize STAI Runtime once (shared across all models) */
  if (!s_runtime_initialized) {
    stai_return_code rt_ret = stai_runtime_init();
    if (rt_ret != STAI_SUCCESS) {
      PRINT("[NNLIB ERROR] STAI Runtime Init Failed: %d\r\n", rt_ret);
      return false;
    }
    s_runtime_initialized = true;
  }

  /* Initialize the AI Model via vtable */
  stai_return_code ret = config->vtable.init(config->network);
  if (ret != STAI_SUCCESS) {
    PRINT("[NNLIB ERROR] Model Init Failed: %d\r\n", ret);
    return false;
  }

  return true;
}

bool nnlib_set_input(nnlib_config_t *config, void *input_data, uint32_t size) {
  stai_ptr inputs[1];
  stai_size num_inputs;

  if (config->vtable.get_inputs(config->network, inputs, &num_inputs) !=
      STAI_SUCCESS) {
    return false;
  }

  if (num_inputs > 0 && inputs[0] != NULL) {
    memcpy(inputs[0], input_data, size);
    return true;
  }

  return false;
}

bool nnlib_run_inference(nnlib_config_t *config, uint32_t *inference_time_ms) {
  uint32_t start_time = HAL_GetTick();

  stai_return_code ret = config->vtable.run(config->network, STAI_MODE_SYNC);

  uint32_t end_time = HAL_GetTick();

  if (inference_time_ms) {
    *inference_time_ms = end_time - start_time;
  }

  if (ret != STAI_SUCCESS) {
    PRINT("[NNLIB ERROR] Inference returned error code: %d\r\n", ret);
  }

  return (ret == STAI_SUCCESS);
}

bool nnlib_get_output(nnlib_config_t *config, void **output_data) {
  stai_ptr outputs[1];
  stai_size num_outputs;

  if (config->vtable.get_outputs(config->network, outputs, &num_outputs) !=
      STAI_SUCCESS) {
    return false;
  }

  if (num_outputs > 0 && output_data != NULL) {
    *output_data = outputs[0];
    return true;
  }

  return false;
}
