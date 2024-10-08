#include "esp32_perfmon.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"
#include "sdkconfig.h"

#include "esp_log.h"
static const char *TAG = "perfmon";

static uint64_t idle0Calls = 0;
static uint64_t idle1Calls = 0;

#if defined(CONFIG_ESP32_DEFAULT_CPU_FREQ_240)
static const uint64_t MaxIdleCalls = 1855000;
#elif defined(CONFIG_ESP32_DEFAULT_CPU_FREQ_160)
static const uint64_t MaxIdleCalls = 1233100;
#else
#error "Unsupported CPU frequency"
#endif

static int cpu0_usage;
static int cpu1_usage;
static bool init_done = false;

bool get_cpu_usage(int *cpu0, int *cpu1)
{
	if (!init_done)
		return false;

	*cpu0 = cpu0_usage;
	*cpu1 = cpu1_usage;
	return true;
}


static bool idle_task_0()
{
	idle0Calls += 1;
	return false;
}

static bool idle_task_1()
{
	idle1Calls += 1;
	return false;
}

static void perfmon_task(void *args)
{
	while (1)
	{
		float idle0 = idle0Calls;
		float idle1 = idle1Calls;
		idle0Calls = 0;
		idle1Calls = 0;

		cpu0_usage = 100.f -  idle0 / MaxIdleCalls * 100.f;
		cpu1_usage = 100.f - idle1 / MaxIdleCalls * 100.f;

		ESP_LOGI(TAG, "Core 0 at %d%%", cpu0_usage);
		ESP_LOGI(TAG, "Core 1 at %d%%", cpu1_usage);
		// TODO configurable delay
		vTaskDelay(5000 / portTICK_PERIOD_MS);
	}
	vTaskDelete(NULL);
}

esp_err_t perfmon_start()
{
	ESP_ERROR_CHECK(esp_register_freertos_idle_hook_for_cpu(idle_task_0, 0));
	ESP_ERROR_CHECK(esp_register_freertos_idle_hook_for_cpu(idle_task_1, 1));
	// TODO calculate optimal stack size
	BaseType_t ret = xTaskCreate(perfmon_task, "perfmon", 2048, NULL, 1, NULL);
	init_done = ret==pdTRUE;

	return ESP_OK;
}