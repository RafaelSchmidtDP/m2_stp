// Esteira Industrial (ESP32 + FreeRTOS) — Sistema Completo com Instrumentação
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/touch_pad.h"
#include "soc/rtc.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"

#include "esp_netif.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>

#define TAG "SISTEMA"

// ======= Variaveis para M2 (Network - SNTP - TCP - UDP) ======= //
static const char *TAG_TIME = "TIME";
static const char *TAG_MONITOR = "MONITOR";

#define WIFI_SSID "Galaxy M53 5G D550"
#define WIFI_PASS "igrt1654"
#define TCP_PORT 5000
#define PC_IP "10.81.100.99" // IP do Servidor/Cliente PC
#define PC_PORT 10422
#define UDP_PORT 10421
#define REPORT_PERIOD_CYCLES 120 // Para relatórios TCP (não usado agora)
#define REPORT_PERIOD_S 60       // 60 segundos para relatório

// 1. Enumeração dos modos de test
typedef enum
{
    MODE_TESTE_TCP,
    MODE_TESTE_UDP
} protocol_mode_t;

// 2. A variável GLOBAL que será alterada para definir o teste
static protocol_mode_t CURRENT_TEST_MODE = MODE_TESTE_UDP;

// ====== Mapeamento dos touch pads ======
#define TP_OBJ TOUCH_PAD_NUM5   // Touch B -> detecção de objeto (desvio) - GPIO 12
#define TP_HMI TOUCH_PAD_NUM6   // Touch C -> HMI/telemetria - GPIO 14
#define TP_ESTOP TOUCH_PAD_NUM7 // Touch D -> E-stop - GPIO 27

// ====== Configurações de tempo ======
#define ENC_T_MS 5 // Período de amostragem do encoder (dt)
#define ENC_DEADLINE_MS 5
#define SORT_DEADLINE_MS 10
#define ESTOP_DEADLINE_MS 5
#define CTRL_DEADLINE_MS 5 // Deadline do controlador PI
#define MONITOR_T_MS 500   // Período de ciclo base da Monitor Task (em ms)

// ====== Prioridades ======
#define PRIO_ESTOP 5
#define PRIO_ENC 4
#define PRIO_CTRL 3
#define PRIO_SORT 2
#define PRIO_MONITOR 1
#define PRIO_TIME 1

// ====== Tamanho de stack ======
#define STK 4096

// ====== Configuração do LED ======
#define LED_GPIO GPIO_NUM_2 // LED interno da ESP32

// ====== Handles/IPC ======
static TaskHandle_t hENC = NULL, hCTRL = NULL, hSORT = NULL, hSAFE = NULL;
static TaskHandle_t hCtrlNotify = NULL;

// Fila de eventos para o SORT_ACT
typedef struct
{
    int64_t t_evt_us; // timestamp do toque (para logging/latência - usado no HMI)
    int64_t t_isr_us; // timestamp da ISR (para cálculo de latência na Task 3)
} sort_evt_t;
static QueueHandle_t qSort = NULL;

// Semáforos para E-stop e HMI
static SemaphoreHandle_t semEStop = NULL;
static SemaphoreHandle_t semHMI = NULL;

// Mutex para proteger stats compartilhadas
static SemaphoreHandle_t mutexStats = NULL;

// ----- Novo: mutex para proteger acesso aos sockets compartilhados -----
static SemaphoreHandle_t mutexSock = NULL;

// ----- Sockets compartilhados entre monitor e report -----
static int g_sock_udp = -1;      // Recebe comandos do PC via UDP e envia logs periódicos de volta.
static int g_tcp_listen_fd = -1; // Socket de escuta TCP (server) para aceitar conexões de um cliente PC.
static int g_client_sock = -1;   // Socket TCP ativo para comunicação com o cliente, criado após accept(). Recebe comandos e envia logs

// Variáveis de desempenho (Misses e Ciclos)
static uint32_t enc_misses = 0;
static uint32_t ctrl_misses = 0;
static uint32_t sort_misses = 0;
static uint32_t estop_misses = 0;
static uint32_t total_enc_cycles = 0;
static uint32_t part_count = 0;

// Variáveis para WCET (Worst-Case Execution Time)
static int64_t wcet_enc = 0;
static int64_t wcet_ctrl = 0;
static int64_t wcet_sort = 0;
static int64_t wcet_safety = 0;
static int64_t wcet_monitor = 0;

// ===== Instrumentação de Tempo Real (WCRT, JITTER) - REMOVIDO BLOCK =====
static int64_t wcrt_enc = 0;
static int64_t wcrt_ctrl = 0;
static int64_t wcrt_sort = 0;
static int64_t wcrt_safety = 0;
static int64_t wcrt_monitor = 0;

static int64_t max_jitter_enc = 0;
static int64_t max_jitter_ctrl = 0;
static int64_t max_jitter_sort = 0;
static int64_t max_jitter_safety = 0;

// AS VARIAVEIS MAX_BLOCK_* FORAM REMOVIDAS
// static int64_t max_block_enc = 0;
// static int64_t max_block_ctrl = 0;
// static int64_t max_block_sort = 0;
// static int64_t max_block_safety = 0;
// static int64_t max_block_monitor = 0;

static int64_t last_release_enc = 0;

// ===== m-k FIRM MODEL (9, 10)-FIRM =====
#define K_FIRM 10 // Janela de k execuções
#define M_FIRM 9  // Mínimo de m sucessos (9 de 10)

// =====================================================================
//                       CORREÇÃO M-K FIRM (INICIALIZAÇÃO)
// =====================================================================

// Estado m-k Firm para ENC_SENSE (REMOVIDO TOTALMENTE)
/*
static bool enc_firm_history[K_FIRM] = {false};
static uint8_t enc_firm_index = 0;
static uint8_t enc_firm_success_count = 0;
*/

// Estado m-k Firm para SPD_CTRL
static bool ctrl_firm_history[K_FIRM] = {false}; // Inicializa com 0 (false)
static uint8_t ctrl_firm_index = 0;
static uint8_t ctrl_firm_success_count = 0; // Começa com 0 sucessos

// Estado m-k Firm para SORT_ACT
static bool sort_firm_history[K_FIRM] = {false}; // Inicializa com 0 (false)
static uint8_t sort_firm_index = 0;
static uint8_t sort_firm_success_count = 0; // Começa com 0 sucessos

// =====================================================================

// ===== Deadlines (em microsegundos) =====
static int64_t deadline_enc_us = 0;
static int64_t deadline_ctrl_us = 0;
static int64_t deadline_sort_us = 0;
static int64_t deadline_safety_us = 0;
static int64_t deadline_time_us = 0;
static int64_t deadline_monitor_us = 0;

// ===== Tempos de CPU acumulados (microsegundos) =====
static int64_t cpu_time_enc_us = 0;
static int64_t cpu_time_ctrl_us = 0;
static int64_t cpu_time_sort_us = 0;
static int64_t cpu_time_safety_us = 0;
static int64_t cpu_time_monitor_us = 0;
static int64_t cpu_time_time_us = 0;

// ===== Armazenamento para cálculo de delta em relatórios =====
static uint32_t last_enc_misses = 0;
static uint32_t last_ctrl_misses = 0;
static uint32_t last_sort_misses = 0;
static uint32_t last_estop_misses = 0;
static int64_t last_cpu_time_total_us = 0;
static int64_t last_report_time_us = 0;

// Estado "simulado" da esteira
typedef struct
{
    float rpm;
    float pos_mm;
    float set_rpm;
} belt_state_t;

static volatile bool g_estop_engaged = false;
static belt_state_t g_belt = {
    .rpm = 0.f,
    .pos_mm = 0.f,
    .set_rpm = 120.0f};

// ====== Util: busy loop previsível (~WCET) ======
static inline void cpu_tight_loop_us(uint32_t us)
{
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < us)
    {
        asm volatile("nop");
    }
}

// ====== Funções Auxiliares (do boilerplate) ======
static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASS);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Conectando ao WiFi...");
    ESP_ERROR_CHECK(esp_wifi_connect());
}

static void IRAM_ATTR touch_isr_handler(void *arg)
{
    uint32_t pad_intr = touch_pad_get_status();
    touch_pad_clear_status();
    int64_t now = esp_timer_get_time();

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (pad_intr & (1 << TP_OBJ))
    {
        sort_evt_t ev = {.t_evt_us = now, .t_isr_us = now};
        xQueueSendFromISR(qSort, &ev, &xHigherPriorityTaskWoken);
    }
    if (pad_intr & (1 << TP_HMI))
    {
        xSemaphoreGiveFromISR(semHMI, &xHigherPriorityTaskWoken);
    }
    if (pad_intr & (1 << TP_ESTOP))
    {
        xSemaphoreGiveFromISR(semEStop, &xHigherPriorityTaskWoken);
    }

    if (xHigherPriorityTaskWoken)
        portYIELD_FROM_ISR();
}

static void touch_pad_init_config(void)
{
    touch_pad_init();
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
    touch_pad_config(TP_OBJ, 0);
    touch_pad_config(TP_HMI, 0);
    touch_pad_config(TP_ESTOP, 0);

    // Calibração automática
    uint16_t val;
    touch_pad_read(TP_OBJ, &val);
    touch_pad_set_thresh(TP_OBJ, val * 2 / 3);
    touch_pad_read(TP_HMI, &val);
    touch_pad_set_thresh(TP_HMI, val * 2 / 3);
    touch_pad_read(TP_ESTOP, &val);
    touch_pad_set_thresh(TP_ESTOP, val * 2 / 3);

    touch_pad_isr_register(touch_isr_handler, NULL);
    touch_pad_intr_enable();
    touch_pad_sw_start();
}

static void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, 0);
}

// SNTP/Time Sync
static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG_TIME, "Tempo sincronizado via SNTP.");
}

static void task_time1(void *arg)
{
    time_t now;
    struct tm timeinfo;
    char buf[64];
    uint32_t cycles_since_sync = 0;

    // ===== Configuração do fuso horário =====
    setenv("TZ", "GMT+3", 1);
    tzset();

    // ===== Inicializa SNTP =====
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "time.google.com");
    esp_sntp_setservername(1, "pool.ntp.br");
    esp_sntp_set_sync_interval(30000); // 30s
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    // ===== Aguarda sincronização inicial =====
    ESP_LOGI(TAG_TIME, "Aguardando sincronização SNTP...");
    int wait_count = 0;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait_count++;
        if (wait_count % 10 == 0) // log a cada 5 s
            ESP_LOGI(TAG_TIME, "Ainda aguardando SNTP...");
    }
    ESP_LOGI(TAG_TIME, "SNTP sincronizado com sucesso.");

    TickType_t next_wake_time = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(1000); // 1 s

    while (1)
    {
        int64_t t_start = esp_timer_get_time();

        // Atualiza hora
        time(&now);
        localtime_r(&now, &timeinfo);
        strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &timeinfo);

        // Contador de ciclos desde última sincronização completa
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED)
            cycles_since_sync = 0;
        else
            cycles_since_sync++;

        ESP_LOGI(TAG_TIME, "Hora de ref: %s | Ciclo: %u", buf, cycles_since_sync);

        int64_t t_end = esp_timer_get_time();
        if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            cpu_time_time_us += (t_end - t_start);
            xSemaphoreGive(mutexStats);
        }

        vTaskDelayUntil(&next_wake_time, period_ticks);
    }
}

// Task 1: task_enc_sense (Periódica - Soft Real-Time) - Métrica Completa
static void task_enc_sense(void *arg)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    TickType_t xPeriod = pdMS_TO_TICKS(ENC_T_MS);
    if (xPeriod == 0)
        xPeriod = 1;
    int64_t t_release, t_start, t_end;

    last_release_enc = esp_timer_get_time();

    while (1)
    {
        // 1. INÍCIO DA EXECUÇÃO (Release) - Medição WCRT/JITTER
        t_release = esp_timer_get_time();

        // CÁLCULO DE JITTER (Desvio do Período)
        int64_t actual_period = t_release - last_release_enc;
        int64_t nominal_period = (int64_t)ENC_T_MS * 1000LL;
        int64_t jitter = llabs(actual_period - nominal_period);

        last_release_enc = t_release;

        // --- INÍCIO DA JANELA DE EXECUÇÃO (WCET) ---
        t_start = esp_timer_get_time();

        // [Leitura do Encoder e Cálculo de RPM]
        cpu_tight_loop_us(150);

        // Notifica o controlador (Task 2)
        // O handle hCtrlNotify agora é garantido de ser válido.
        xTaskNotify(hCtrlNotify, 0, eNoAction);

        total_enc_cycles++;
        g_belt.pos_mm += g_belt.rpm * ((float)ENC_T_MS / 1000.0f);

        // =============================================================
        //               CÁLCULO DE MÉTRICA (WCET/WCRT)
        // =============================================================

        // --- FIM DA JANELA DE EXECUÇÃO (WCET) ---
        t_end = esp_timer_get_time(); // <-- TEMPO FINAL MEDIDO ANTES DO MUTEX

        // CÁLCULO DE WCET: Duração do C
        int64_t execution_time = t_end - t_start;
        // CÁLCULO DE WCRT: Duração R (t_end - t_release)
        int64_t response_time = t_end - t_release;

        // 2. ATUALIZAÇÃO DO ESTADO GLOBAL
        if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            // REMOVIDO: Lógica max_block_enc

            // --- ATUALIZAÇÃO DAS STATS  ---
            if (execution_time > wcet_enc)
                wcet_enc = execution_time;
            if (response_time > wcrt_enc)
                wcrt_enc = response_time;

            // Atualiza Jitter (medido no início)
            if (jitter > max_jitter_enc)
                max_jitter_enc = jitter;

            // Determina se houve sucesso ou falha no deadline (baseado em WCRT)
            bool success = (response_time <= deadline_enc_us);

            // LÓGICA M-K FIRM
            if (!success)
                enc_misses++;
            /*
            if (!success)
                enc_misses++;

            if (enc_firm_history[enc_firm_index] && !success) // Era Sucesso (T), virou Falha (F)
            {
                enc_firm_success_count--;
            }
            else if (!enc_firm_history[enc_firm_index] && success) // Era Falha (F), virou Sucesso (T)
            {
                enc_firm_success_count++;
            }

            enc_firm_history[enc_firm_index] = success;
            enc_firm_index = (enc_firm_index + 1) % K_FIRM; // vetor circular
            */

            cpu_time_enc_us += execution_time;

            xSemaphoreGive(mutexStats);
        }
        else
        {
            enc_misses++; // Conta MISS (falha ao obter mutex)
        }

        vTaskDelayUntil(&xLastWakeTime, xPeriod);
    }
}

// Task 2: task_spd_ctrl (Notification-driven - Controlador PI)
static void task_spd_ctrl(void *arg)
{
    static float kp = 0.4f, ki = 0.1f, integ = 0.f;
    int64_t t_release, t_start, t_end;

    for (;;)
    {
        // 1. BLOQUEIO E INÍCIO DA EXECUÇÃO (Block Time)
        // int64_t block_start = esp_timer_get_time(); //

        // BLOQUEIO: Espera por notificação do ENC_SENSE
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int64_t block_end = esp_timer_get_time();
        // int64_t block_duration = block_end - block_start; //

        // REMOVIDO: Lógica max_block_ctrl
        // if (block_duration > max_block_ctrl)
        //     max_block_ctrl = block_duration;

        t_release = block_end; // t_release é o fim do bloqueio (chegada da notificação)

        // --- INÍCIO DA JANELA DE EXECUÇÃO (WCET) ---
        t_start = esp_timer_get_time();

        // Lógica PI
        if (g_estop_engaged)
        {
            integ = 0.f;
            g_estop_engaged = false;
        }

        float err = g_belt.set_rpm - g_belt.rpm;
        integ += err * ((float)ENC_T_MS / 1000.0f);
        float u = kp * err + ki * integ;
        g_belt.rpm += 0.05f * u;

        cpu_tight_loop_us(1200); // Simulação do WCET do controlador

        // Checagem HMI
        if (xSemaphoreTake(semHMI, 0) == pdTRUE)
        {
            ESP_LOGI(TAG, "[TOUCH C] Telemetria | Vel: %.1f RPM | Peças: %lu", g_belt.rpm, part_count);
            cpu_tight_loop_us(400);
        }

        // =============================================================
        //               CÁLCULO DE MÉTRICA (WCET/WCRT)
        // =============================================================

        // --- FIM DA JANELA DE EXECUÇÃO (WCET) ---
        t_end = esp_timer_get_time(); // <-- TEMPO FINAL MEDIDO ANTES DO MUTEX
        int64_t execution_time = t_end - t_start;

        // CÁLCULO DE WCRT (Response Time): t_end - t_release
        int64_t response_time = t_end - t_release;

        // 2. ATUALIZAÇÃO DO ESTADO GLOBAL (Stats)
        if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            // REMOVIDO: Atualiza tempo de bloqueio (max_block_ctrl)

            // CÁLCULO DE WCET
            if (execution_time > wcet_ctrl)
                wcet_ctrl = execution_time;

            // CÁLCULO DE WCRT
            if (response_time > wcrt_ctrl)
                wcrt_ctrl = response_time;

            // Para tarefas event-driven, o Response Time é a melhor medida de Jitter (variância)
            if (response_time > max_jitter_ctrl)
                max_jitter_ctrl = response_time;

            // Determina se houve sucesso ou falha no deadline (baseado em WCRT)
            bool success = (response_time <= deadline_ctrl_us);

            // LÓGICA M-K FIRM
            if (!success)
                ctrl_misses++;
            // Se a execução antiga era sucesso e agora falhou -> diminui o contador.
            // Se a execução antiga era falha e agora foi sucesso -> aumenta o contador.
            if (ctrl_firm_history[ctrl_firm_index] && !success) // T -> F
            {
                ctrl_firm_success_count--;
            }
            else if (!ctrl_firm_history[ctrl_firm_index] && success) // F -> T
            {
                ctrl_firm_success_count++;
            }
            // gaurda resultado no vetor
            ctrl_firm_history[ctrl_firm_index] = success;
            // move pra proxima posição
            ctrl_firm_index = (ctrl_firm_index + 1) % K_FIRM;

            cpu_time_ctrl_us += execution_time;
            xSemaphoreGive(mutexStats);
        }
        else
        {
            ctrl_misses++;
        }
    }
}

// Task 3: task_sort_act (Event-driven)
static void task_sort_act(void *arg)
{
    sort_evt_t ev;
    int64_t t_release, t_start, t_end;
    for (;;)
    {
        // 1. BLOQUEIO E INÍCIO DA EXECUÇÃO (Block Time)
        // int64_t block_start = esp_timer_get_time(); //

        // BLOQUEIO: Espera na fila (Queue)
        if (xQueueReceive(qSort, &ev, portMAX_DELAY) == pdTRUE)
        {
            int64_t block_end = esp_timer_get_time();

            // REMOVIDO: CÁLCULO DE BLOCK TIME
            // int64_t block_duration = block_end - block_start;

            t_release = block_end; // t_release é o fim do bloqueio

            // --- INÍCIO DA JANELA DE EXECUÇÃO (WCET) ---
            t_start = esp_timer_get_time();

            part_count++;
            // Latência ISR -> Início da Execução da Task
            int64_t latency = t_release - ev.t_isr_us;

            // Simulação de execução
            cpu_tight_loop_us(900);

            // --- FIM DA JANELA DE EXECUÇÃO (WCET) ---
            t_end = esp_timer_get_time();              //
            int64_t duration = t_end - t_start;        // Tempo de CPU (WCET)
            int64_t response_time = t_end - t_release; // Tempo de Resposta (WCRT)

            // 2. ATUALIZAÇÃO DO ESTADO GLOBAL (Stats)
            if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE)
            {
                // REMOVIDO: Lógica max_block_sort
                // if (block_duration > max_block_sort)
                //     max_block_sort = block_duration;

                if (response_time > max_jitter_sort)
                    max_jitter_sort = response_time;

                // CÁLCULO DE WCET
                if (duration > wcet_sort)
                    wcet_sort = duration;

                // CÁLCULO DE WCRT
                if (response_time > wcrt_sort)
                    wcrt_sort = response_time;

                cpu_time_sort_us += duration;

                // Checa Miss: BASEADO EM WCRT
                bool success = (response_time <= deadline_sort_us);

                // LÓGICA M-K FIRM
                if (!success)
                    sort_misses++; // Conta miss

                if (sort_firm_history[sort_firm_index] && !success) // T -> F
                {
                    sort_firm_success_count--;
                }
                else if (!sort_firm_history[sort_firm_index] && success) // F -> T
                {
                    sort_firm_success_count++;
                }

                sort_firm_history[sort_firm_index] = success;
                sort_firm_index = (sort_firm_index + 1) % K_FIRM;

                if (!success)
                {
                    ESP_LOGW(TAG, "SORT miss (WCRT): R=%lld us (dl=%lld) total=%lu",
                             (long long)response_time, (long long)deadline_sort_us, sort_misses);
                }
                xSemaphoreGive(mutexStats);
            }

            ESP_LOGI(TAG, "[TOUCH B] Objeto detectado | Lat ISR→Task=%.2f ms | Total peças=%lu",
                     latency / 1000.0, part_count);
        }
    }
}

// Task 4: task_safety (Event-driven)
static void task_safety(void *arg)
{
    int64_t t_release, t_start, t_end;
    for (;;)
    {
        // 1. BLOQUEIO E INÍCIO DA EXECUÇÃO (Block Time)
        // int64_t block_start = esp_timer_get_time(); //

        // BLOQUEIO: Espera no semáforo (E-stop)
        if (xSemaphoreTake(semEStop, portMAX_DELAY) == pdTRUE)
        {
            int64_t block_end = esp_timer_get_time();

            // REMOVIDO: CÁLCULO DE BLOCK TIME
            // int64_t block_duration = block_end - block_start;

            t_release = block_end; // t_release é o fim do bloqueio

            // --- INÍCIO DA JANELA DE EXECUÇÃO (WCET) ---
            t_start = esp_timer_get_time();

            // Lógica E-stop
            g_belt.rpm = 0.f;
            g_belt.set_rpm = 0.f;
            g_estop_engaged = true;

            ESP_LOGI(TAG, "[TOUCH D] E-stop acionado | Sistema totalmente zerado");

            // Simulação de execução
            cpu_tight_loop_us(900);

            // --- FIM DA JANELA DE EXECUÇÃO (WCET) ---
            t_end = esp_timer_get_time();              //
            int64_t duration = t_end - t_start;        // Tempo de CPU (WCET)
            int64_t response_time = t_end - t_release; // Tempo de Resposta (WCRT)

            // 2. ATUALIZAÇÃO DO ESTADO GLOBAL (Stats)
            if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE)
            {
                // REMOVIDO: Lógica max_block_safety
                // if (block_duration > max_block_safety)
                //     max_block_safety = block_duration;

                if (response_time > max_jitter_safety)
                    max_jitter_safety = response_time;

                // CÁLCULO DE WCET
                if (duration > wcet_safety)
                    wcet_safety = duration;

                // CÁLCULO DE WCRT
                if (response_time > wcrt_safety)
                    wcrt_safety = response_time;

                cpu_time_safety_us += duration;

                // Checa Miss: BASEADO EM WCRT
                if (response_time > deadline_safety_us)
                {
                    estop_misses++;
                    ESP_LOGW(TAG, "SAFETY miss (WCRT): R=%lld us (dl=%lld) total=%lu",
                             (long long)response_time, (long long)deadline_safety_us, estop_misses);
                }
                xSemaphoreGive(mutexStats);
            }
        }
    }
}

// ======== TASK: MONITORAMENTO E COMUNICAÇÃO PC ↔ ESP32 ========
static void task_monitor(void *arg)
{
    // Recebe o modo de protocolo (UDP ou TCP) definido em 'app_main'.
    protocol_mode_t *current_mode = (protocol_mode_t *)arg;

    // Sockets temporários locais usados para a inicialização.
    int sock_udp = -1;
    int tcp_listen_fd = -1;

    // I. CONFIGURAÇÃO INICIAL DO SOCKET (FORA DO LOOP)

    // Configura sockets conforme modo
    if (*current_mode == MODE_TESTE_UDP)
    {
        // 1. Cria o socket UDP: AF_INET (IPv4), SOCK_DGRAM (Datagrama).
        sock_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock_udp < 0)
        {
            ESP_LOGE(TAG_MONITOR, "Erro UDP");
            vTaskDelete(NULL);
        }

        // 2. Define o endereço local para RECEBER dados
        struct sockaddr_in local_addr = {0};
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(UDP_PORT);          // Define a porta local para escuta (10421).
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Escuta em qualquer endereço (servidor UDP)

        // 3. Vincula (bind) o socket à porta e endereço local.
        if (bind(sock_udp, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
        {
            ESP_LOGE(TAG_MONITOR, "Erro bind UDP (%d)", errno);
            close(sock_udp);
            vTaskDelete(NULL);
        }

        // 4. Salva o socket no descritor global (g_sock_udp) protegido por Mutex para acesso por outras tasks.
        xSemaphoreTake(mutexSock, portMAX_DELAY);
        g_sock_udp = sock_udp;
        xSemaphoreGive(mutexSock);

        ESP_LOGI(TAG_MONITOR, "INICIANDO MODO UDP");
    }
    else if (*current_mode == MODE_TESTE_TCP)
    {
        // 1. Cria o socket de escuta TCP: SOCK_STREAM (Orientado à conexão).
        tcp_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (tcp_listen_fd < 0)
        {
            ESP_LOGE(TAG_MONITOR, "Falha TCP Listen (%d)", errno);
            vTaskDelete(NULL);
        }

        // 2. Configura SO_REUSEADDR para reuso imediato da porta (5000) após fechamento, evitando TIME_WAIT.
        // reutilize o mesmo endereço de porta imediatamente após ser fechado ou após uma falha
        int opt = 1;
        setsockopt(tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // 3. Define o endereço local (servidor TCP)
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(TCP_PORT); // Porta local de escuta (5000).
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        // 4. Vincula (bind) o socket à porta e endereço local.
        if (bind(tcp_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            ESP_LOGE(TAG_MONITOR, "Erro bind TCP (%d)", errno);
            close(tcp_listen_fd);
            vTaskDelete(NULL);
        }

        // 5. Entra no modo de escuta (listen), aguardando conexões (fila de 1 pendente).
        if (listen(tcp_listen_fd, 1) < 0)
        {
            ESP_LOGE(TAG_MONITOR, "Erro listen TCP (%d)", errno);
            close(tcp_listen_fd);
            vTaskDelete(NULL);
        }

        // 6. Salva o socket de escuta no descritor global (g_tcp_listen_fd).
        xSemaphoreTake(mutexSock, portMAX_DELAY);
        g_tcp_listen_fd = tcp_listen_fd;
        xSemaphoreGive(mutexSock);

        ESP_LOGI(TAG_MONITOR, "INICIANDO MODO TCP (recv não-bloqueante com MSG_DONTWAIT)");
    }

    if (last_report_time_us == 0)
        last_report_time_us = esp_timer_get_time();

    TickType_t next_wake_time = xTaskGetTickCount();
    const TickType_t xPeriod = pdMS_TO_TICKS(MONITOR_T_MS); // Período base (500ms).
    int seq = 0;

    char rx_buf[128]; // Buffer para recebimento de comandos do PC
    char tx_buf[512]; // Buffer para envio de logs periódicos

    // II. LOOP PERIÓDICO DE COMUNICAÇÃO (500ms)

    while (1)
    {
        int64_t t_release = esp_timer_get_time(); // Tempo de liberação da Task (para WCRT)
        int64_t t_start, t_end;
        int len = 0;

        // === 1. ACEITAR CONEXÃO TCP ===
        if (*current_mode == MODE_TESTE_TCP)
        {
            // Tenta aceitar nova conexão APENAS se não houver cliente ativo (g_client_sock < 0).
            if (g_client_sock < 0)
            {
                xSemaphoreTake(mutexSock, portMAX_DELAY);
                int listen_fd = g_tcp_listen_fd;
                xSemaphoreGive(mutexSock);

                if (listen_fd >= 0)
                {
                    struct sockaddr_in source_addr;
                    socklen_t addr_len = sizeof(source_addr);
                    // accept(): Aguarda um cliente. Essa função pode bloquear o sistema se não houver cliente e a tarefa for preemptiva.
                    int sock = accept(listen_fd, (struct sockaddr *)&source_addr, &addr_len);
                    if (sock >= 0)
                    {
                        // Novo socket ('sock') criado para a comunicação ATIVA com o cliente
                        xSemaphoreTake(mutexSock, portMAX_DELAY);
                        g_client_sock = sock;
                        xSemaphoreGive(mutexSock);
                        ESP_LOGI(TAG_MONITOR, "Cliente TCP conectado");
                    }
                }
            }
        }

        t_start = esp_timer_get_time(); // Início da execução (para WCET)

        // === 2. RECEBER COMANDOS (PC → ESP32) ===
        if (*current_mode == MODE_TESTE_UDP)
        {
            xSemaphoreTake(mutexSock, portMAX_DELAY);
            int s_udp = g_sock_udp;
            xSemaphoreGive(mutexSock);

            if (s_udp >= 0)
            {
                struct sockaddr_in sender_addr;
                socklen_t addr_len = sizeof(sender_addr);

                // recvfrom: Usa MSG_DONTWAIT para tornar a leitura NÃO-BLOQUEANTE (non-blocking).
                // 'sender_addr' armazena o endereço do PC que enviou o comando (para resposta PONG).
                len = recvfrom(s_udp, rx_buf, sizeof(rx_buf) - 1, MSG_DONTWAIT,
                               (struct sockaddr *)&sender_addr, &addr_len);

                if (len > 0)
                {
                    rx_buf[len] = 0;
                    ESP_LOGI(TAG_MONITOR, "Comando recebido: %s", rx_buf);

                    // Lógica para processar comandos (PING, SAFETY_ON, SORT_ACT)
                    if (strncmp(rx_buf, "PING", 4) == 0)
                    {
                        int tx_len = snprintf(tx_buf, sizeof(tx_buf), "PONG %lld", esp_timer_get_time());
                        // Envia PONG de volta para o 'sender_addr' que enviou o PING.
                        sendto(s_udp, tx_buf, tx_len, 0, (struct sockaddr *)&sender_addr, addr_len);
                    }
                    else if (strncmp(rx_buf, "SAFETY_ON", 9) == 0)
                    {
                        xSemaphoreGive(semEStop); // Libera o semáforo de E-Stop (emergência).
                        gpio_set_level(LED_GPIO, 1);
                    }
                    else if (strncmp(rx_buf, "SORT_ACT", 8) == 0)
                    {
                        sort_evt_t ev = {.t_evt_us = esp_timer_get_time(), .t_isr_us = 0};
                        xQueueSend(qSort, &ev, 0); // Envia evento para a task_sort_act.
                    }
                }
            }
        }
        else if (*current_mode == MODE_TESTE_TCP)
        {
            xSemaphoreTake(mutexSock, portMAX_DELAY);
            int c_sock = g_client_sock;
            xSemaphoreGive(mutexSock);

            if (c_sock >= 0) // Se há cliente conectado...
            {
                // recv: Usa MSG_DONTWAIT para leitura NÃO-BLOQUEANTE.
                len = recv(c_sock, rx_buf, sizeof(rx_buf) - 1, MSG_DONTWAIT);

                if (len == 0) // len == 0: Cliente TCP desconectou.
                {
                    ESP_LOGI(TAG_MONITOR, "Cliente TCP desconectado (len==0)");
                    // Fecha e reseta o socket ativo
                    xSemaphoreTake(mutexSock, portMAX_DELAY);
                    close(g_client_sock);
                    g_client_sock = -1;
                    xSemaphoreGive(mutexSock);
                    len = 0;
                }
                else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) // Outro erro
                {
                    ESP_LOGE(TAG_MONITOR, "Erro TCP recv (%d)", errno);
                    // Erro: fecha o socket ativo
                    xSemaphoreTake(mutexSock, portMAX_DELAY);
                    close(g_client_sock);
                    g_client_sock = -1;
                    xSemaphoreGive(mutexSock);
                    len = 0;
                }
                else if (len > 0)
                {
                    rx_buf[len] = 0;
                    ESP_LOGI(TAG_MONITOR, "Comando recebido: %s", rx_buf);

                    // Processa comandos linha por linha (necessário para TCP).
                    char *line = strtok(rx_buf, "\n");
                    while (line != NULL)
                    {
                        if (strncmp(line, "PING", 4) == 0 && c_sock >= 0)
                        {
                            int tx_len = snprintf(tx_buf, sizeof(tx_buf), "PONG %lld\n", esp_timer_get_time());
                            send(c_sock, tx_buf, tx_len, 0); // Responde PONG via TCP
                        }
                        else if (strncmp(line, "SAFETY_ON", 9) == 0)
                        {
                            xSemaphoreGive(semEStop);
                            gpio_set_level(LED_GPIO, 1);
                        }
                        else if (strncmp(line, "SORT_ACT", 8) == 0)
                        {
                            sort_evt_t ev = {.t_evt_us = esp_timer_get_time(), .t_isr_us = 0};
                            xQueueSend(qSort, &ev, 0);
                        }

                        line = strtok(NULL, "\n");
                    }
                }
            }
        }

        // === 3. ENVIAR LOGS PERIÓDICOS (ESP32 → PC) ===
        // Obtém o timestamp atual do SNTP para o log.
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        int64_t sntp_timestamp_us = ((int64_t)tv_now.tv_sec * 1000000LL) + (int64_t)tv_now.tv_usec;

        // Formata a mensagem JSON com os dados (RPM, contagem, estop, timestamp).
        int tx_off = snprintf(tx_buf, sizeof(tx_buf),
                              "{\"protocol\":\"%s\",\"seq\":%d,\"rpm\":%.1f,\"count\":%lu,"
                              "\"estop\":%s,\"ts_us\":%lld}\n",
                              (*current_mode == MODE_TESTE_UDP ? "UDP" : "TCP"),
                              seq++, g_belt.rpm, part_count,
                              g_estop_engaged ? "true" : "false", (long long)sntp_timestamp_us);

        if (*current_mode == MODE_TESTE_UDP)
        {
            xSemaphoreTake(mutexSock, portMAX_DELAY);
            int s_udp = g_sock_udp;
            xSemaphoreGive(mutexSock);

            if (s_udp >= 0)
            {
                // Configura o endereço de DESTINO fixo do PC (PC_IP: 10.81.100.99, PC_PORT: 10422).
                struct sockaddr_in dest_addr = {0};
                dest_addr.sin_family = AF_INET;
                dest_addr.sin_port = htons(PC_PORT);
                inet_pton(AF_INET, PC_IP, &dest_addr.sin_addr);

                // sendto: Envia o datagrama (log) para o endereço de destino (PC).
                int sent = sendto(s_udp, tx_buf, tx_off, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                if (sent < 0)
                    ESP_LOGE(TAG_MONITOR, "Erro sendto UDP (%d)", errno);
            }
        }
        else if (*current_mode == MODE_TESTE_TCP)
        {
            xSemaphoreTake(mutexSock, portMAX_DELAY);
            int c_sock = g_client_sock;
            xSemaphoreGive(mutexSock);

            if (c_sock >= 0)
            {
                // send: Envia o log através da conexão TCP.
                int sent = send(c_sock, tx_buf, tx_off, 0);
                if (sent < 0)
                {
                    // Se o envio falhar (ex: conexão resetada), fecha o socket para permitir nova conexão.
                    ESP_LOGE(TAG_MONITOR, "Erro send TCP (%d) — fechando cliente", errno);
                    xSemaphoreTake(mutexSock, portMAX_DELAY);
                    close(g_client_sock);
                    g_client_sock = -1;
                    xSemaphoreGive(mutexSock);
                }
            }
        }

        cpu_tight_loop_us(500); // Simulação de uso de CPU.

        t_end = esp_timer_get_time();
        int64_t duration = t_end - t_start;        // Duração da execução (WCET)
        int64_t response_time = t_end - t_release; // Tempo total de resposta (WCRT)

        // 4. ATUALIZAÇÃO DE MÉTRICAS (WCET/WCRT)
        // Atualiza as estatísticas globais (protegidas por mutexStats).
        if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            if (duration > wcet_monitor)
                wcet_monitor = duration;
            if (response_time > wcrt_monitor)
                wcrt_monitor = response_time;

            cpu_time_monitor_us += duration;
            xSemaphoreGive(mutexStats);
        }

        // 5. Bloqueia a task até o próximo ciclo periódico de 500ms
        vTaskDelayUntil(&next_wake_time, xPeriod);
    }

    // =======================================================
    // V. ENCERRAMENTO DA TASK (Cleanup - se a task for deletada)
    // =======================================================

    // Fecha todos os sockets globais antes de sair, protegido por mutex.
    xSemaphoreTake(mutexSock, portMAX_DELAY);
    if (g_tcp_listen_fd > 0)
    {
        close(g_tcp_listen_fd);
        g_tcp_listen_fd = -1;
    }
    if (g_sock_udp > 0)
    {
        close(g_sock_udp);
        g_sock_udp = -1;
    }
    if (g_client_sock > 0)
    {
        close(g_client_sock);
        g_client_sock = -1;
    }
    xSemaphoreGive(mutexSock);

    vTaskDelete(NULL); // Deleta a própria task
}

// ======== TASK: relatório de 60s (separada) ========
static void task_report_60s(void *arg)
{
    protocol_mode_t *current_mode = (protocol_mode_t *)arg;

    TickType_t next_wake_time = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(REPORT_PERIOD_S * 1000);

    while (1)
    {
        vTaskDelayUntil(&next_wake_time, period_ticks);

        int64_t now_us = esp_timer_get_time();

        // Coleta métricas atuais protegidas por mutexStats
        uint32_t enc_misses_now = 0, ctrl_misses_now = 0, sort_misses_now = 0, estop_misses_now = 0;
        uint8_t ctrl_firm_suc_now = 0; //
        uint8_t sort_firm_suc_now = 0; // s
        int64_t cpu_total_now = 0;

        // Variáveis locais para snapshot dos WCET/WCRT
        int64_t wcet_enc_snap = 0, wcet_ctrl_snap = 0, wcet_sort_snap = 0, wcet_safety_snap = 0, wcet_monitor_snap = 0;
        int64_t wcrt_enc_snap = 0, wcrt_ctrl_snap = 0, wcrt_sort_snap = 0, wcrt_safety_snap = 0, wcrt_monitor_snap = 0;
        int64_t jitter_enc_snap = 0, jitter_ctrl_snap = 0, jitter_sort_snap = 0, jitter_safety_snap = 0;
        // REMOVIDO: max_block_* snapshots
        // int64_t block_enc_snap = 0, block_ctrl_snap = 0, block_sort_snap = 0, block_safety_snap = 0, block_monitor_snap = 0;

        if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            // Copia misses
            enc_misses_now = enc_misses;
            ctrl_misses_now = ctrl_misses;
            sort_misses_now = sort_misses;
            estop_misses_now = estop_misses;

            // Copia contadores m-k
            ctrl_firm_suc_now = ctrl_firm_success_count;
            sort_firm_suc_now = sort_firm_success_count;

            // Copia tempos de CPU
            cpu_total_now = cpu_time_enc_us + cpu_time_ctrl_us + cpu_time_sort_us +
                            cpu_time_safety_us + cpu_time_monitor_us + cpu_time_time_us;

            // Copia métricas de "pior caso" (WCET, WCRT, etc.)
            wcet_enc_snap = wcet_enc;
            wcet_ctrl_snap = wcet_ctrl;
            wcet_sort_snap = wcet_sort;
            wcet_safety_snap = wcet_safety;
            wcet_monitor_snap = wcet_monitor;
            wcrt_enc_snap = wcrt_enc;
            wcrt_ctrl_snap = wcrt_ctrl;
            wcrt_sort_snap = wcrt_sort;
            wcrt_safety_snap = wcrt_safety;
            wcrt_monitor_snap = wcrt_monitor;
            jitter_enc_snap = max_jitter_enc;
            jitter_ctrl_snap = max_jitter_ctrl;
            jitter_sort_snap = max_jitter_sort;
            jitter_safety_snap = max_jitter_safety;
            // REMOVIDO: block snapshots
            // block_enc_snap = max_block_enc;
            // block_ctrl_snap = max_block_ctrl;
            // block_sort_snap = max_block_sort;
            // block_safety_snap = max_block_safety;
            // block_monitor_snap = max_block_monitor;

            // ZERA as métricas de "pior caso" para a próxima janela de 60s
            wcet_enc = wcet_ctrl = wcet_sort = wcet_safety = wcet_monitor = 0;
            wcrt_enc = wcrt_ctrl = wcrt_sort = wcrt_safety = wcrt_monitor = 0;
            max_jitter_enc = max_jitter_ctrl = max_jitter_sort = max_jitter_safety = 0;
            // REMOVIDO: Zera max_block_*
            // max_block_enc = max_block_ctrl = max_block_sort = max_block_safety = max_block_monitor = 0;

            // ZERA os acumuladores de tempo de CPU
            cpu_time_enc_us = cpu_time_ctrl_us = cpu_time_sort_us =
                cpu_time_safety_us = cpu_time_monitor_us = cpu_time_time_us = 0;

            xSemaphoreGive(mutexStats);
        }
        else
        {
            ESP_LOGW(TAG_MONITOR, "Falha ao obter mutexStats para relatório");
            continue;
        }

        // Calcula deltas e porcentagem CPU
        int64_t period_us = now_us - last_report_time_us;
        // int64_t cpu_delta_us = cpu_total_now - last_cpu_time_total_us; // 'last_cpu_time_total_us' será 0 na primeira vez
        float cpu_pct = period_us > 0 ? (float)cpu_total_now * 100.0f / (float)period_us : 0.0f; // Usa o 'cpu_total_now'

        uint32_t enc_miss_delta = enc_misses_now - last_enc_misses;
        uint32_t ctrl_miss_delta = ctrl_misses_now - last_ctrl_misses;
        uint32_t sort_miss_delta = sort_misses_now - last_sort_misses;
        uint32_t estop_miss_delta = estop_misses_now - last_estop_misses;

        // Monta relatório (LINHA M-K FIRM ALTERADA)
        char report_buf[1024];
        int off = snprintf(report_buf, sizeof(report_buf),
                           "=== REPORT %s (period %lld us) ===\n"
                           "WCET (us): ENC=%lld | CTRL=%lld | SORT=%lld | SAFETY=%lld | MON=%lld\n"
                           "WCRT (us): ENC=%lld | CTRL=%lld | SORT=%lld | SAFETY=%lld | MON=%lld\n"
                           "JITTER (us): ENC=%lld | CTRL=%lld | SORT=%lld | SAFETY=%lld\n"
                           // REMOVIDA A LINHA BLOCK
                           "Misses: ENC=%lu | CTRL=%lu | SORT=%lu | SAFETY=%lu\n"
                           "m-k FIRM (%d/%d): CTRL=%d | SORT=%d\n"
                           "Counts: parts=%lu | enc_cycles=%lu\n"
                           "CPU period = %.2f%%\n"
                           "Belt: rpm=%.1f set=%.1f pos=%.1fmm\n"
                           "============================\n",
                           (*current_mode == MODE_TESTE_UDP ? "UDP" : "TCP"),
                           (long long)period_us,
                           (long long)wcet_enc_snap, (long long)wcet_ctrl_snap, (long long)wcet_sort_snap,
                           (long long)wcet_safety_snap, (long long)wcet_monitor_snap,
                           (long long)wcrt_enc_snap, (long long)wcrt_ctrl_snap, (long long)wcrt_sort_snap,
                           (long long)wcrt_safety_snap, (long long)wcrt_monitor_snap,
                           (long long)jitter_enc_snap, (long long)jitter_ctrl_snap,
                           (long long)jitter_sort_snap, (long long)jitter_safety_snap,
                           enc_miss_delta, ctrl_miss_delta, sort_miss_delta, estop_miss_delta,
                           M_FIRM, K_FIRM, ctrl_firm_suc_now, sort_firm_suc_now,
                           part_count, total_enc_cycles,
                           cpu_pct, g_belt.rpm, g_belt.set_rpm, g_belt.pos_mm);

        // Envia relatório usando o mesmo protocolo ativo
        if (*current_mode == MODE_TESTE_UDP)
        {
            xSemaphoreTake(mutexSock, portMAX_DELAY);
            int s_udp = g_sock_udp;
            xSemaphoreGive(mutexSock);

            if (s_udp >= 0)
            {
                struct sockaddr_in dest_addr = {0};
                dest_addr.sin_family = AF_INET;
                dest_addr.sin_port = htons(PC_PORT);
                inet_pton(AF_INET, PC_IP, &dest_addr.sin_addr);
                int sent = sendto(s_udp, report_buf, off, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                if (sent < 0)
                    ESP_LOGE(TAG_MONITOR, "Erro sendto UDP (report) (%d)", errno);
                else
                    ESP_LOGI(TAG_MONITOR, "Relatório UDP enviado (%.2f%% CPU)", cpu_pct);
            }
            else
            {
                ESP_LOGW(TAG_MONITOR, "Socket UDP não disponível para relatório");
            }
        }
        else // TCP
        {
            xSemaphoreTake(mutexSock, portMAX_DELAY);
            int c_sock = g_client_sock;
            xSemaphoreGive(mutexSock);

            if (c_sock >= 0)
            {
                int sent = send(c_sock, report_buf, off, MSG_DONTWAIT);
                if (sent < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        ESP_LOGW(TAG_MONITOR, "TCP send (report) buffer cheio -> drop report");
                    }
                    else
                    {
                        ESP_LOGE(TAG_MONITOR, "Erro send TCP (report) (%d) -> fechando cliente", errno);
                        xSemaphoreTake(mutexSock, portMAX_DELAY);
                        close(g_client_sock);
                        g_client_sock = -1;
                        xSemaphoreGive(mutexStats);
                    }
                }
                else
                {
                    ESP_LOGI(TAG_MONITOR, "Relatório TCP enviado (%.2f%% CPU)", cpu_pct);
                }
            }
            else
            {
                ESP_LOGW(TAG_MONITOR, "Nenhum cliente TCP conectado para relatório");
            }
        }

        // Atualiza acumuladores (guardados em mutexStats)
        last_report_time_us = now_us;
        last_cpu_time_total_us = cpu_total_now; // Salva o total que foi zerado
        last_enc_misses = enc_misses_now;
        last_ctrl_misses = ctrl_misses_now;
        last_sort_misses = sort_misses_now;
        last_estop_misses = estop_misses_now;
    }
}

// ====== app_main ======
void app_main(void)
{
    // === 1. Inicialização de Sistema e NVS (Obrigatório para WiFi) ===
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inicialização LED
    led_init();
    ESP_LOGI(TAG, "Esteira industrial inicializada.");

    // === 2. Inicialização de IPCs e Semáforos ===
    qSort = xQueueCreate(10, sizeof(sort_evt_t));
    semEStop = xSemaphoreCreateBinary();
    semHMI = xSemaphoreCreateBinary();
    mutexStats = xSemaphoreCreateMutex();
    mutexSock = xSemaphoreCreateMutex();
    if (!qSort || !semEStop || !semHMI || !mutexStats || !mutexSock)
    {
        ESP_LOGE(TAG, "Falha ao criar IPCs");
        return;
    }

    // === 3. Inicialização do WiFi e Touch Pads ===
    wifi_init();
    touch_pad_init_config();

    // === 4. Inicializa Deadlines em microsegundos ===
    deadline_enc_us = (int64_t)ENC_DEADLINE_MS * 1000LL;
    deadline_ctrl_us = (int64_t)CTRL_DEADLINE_MS * 1000LL;
    deadline_sort_us = (int64_t)SORT_DEADLINE_MS * 1000LL;
    deadline_safety_us = (int64_t)ESTOP_DEADLINE_MS * 1000LL;
    deadline_time_us = 1000LL * 1000LL;
    deadline_monitor_us = (int64_t)MONITOR_T_MS * 1000LL;

    ESP_LOGI(TAG, "Deadlines: ENC=%lldus | CTRL=%lldus | SORT=%lldus | ESTOP=%lldus | MON=%lldus",
             (long long)deadline_enc_us, (long long)deadline_ctrl_us, (long long)deadline_sort_us, (long long)deadline_safety_us, (long long)deadline_monitor_us);

    // === 5. Escolha do cenário de teste ===
    // MODE_TESTE_TCP ou MODE_TESTE_UDP
    CURRENT_TEST_MODE = MODE_TESTE_UDP;
    ESP_LOGI(TAG, "Modo de Teste: %s", (CURRENT_TEST_MODE == MODE_TESTE_UDP) ? "UDP" : "TCP");
    ESP_LOGI(TAG, "m-k FIRM: (%d, %d)", M_FIRM, K_FIRM);

    // === 6. Criação das Tasks (ORDEM CORRIGIDA) ===

    // Core 0: Tasks de alta prioridade (Control e Safety)
    xTaskCreatePinnedToCore(task_safety, "SAFETY", STK, NULL, PRIO_ESTOP, &hSAFE, 0);
    xTaskCreatePinnedToCore(task_spd_ctrl, "SPD_CTRL", STK, NULL, PRIO_CTRL, &hCTRL, 0);
    hCtrlNotify = hCTRL; // Handle já atribuído
    xTaskCreatePinnedToCore(task_enc_sense, "ENC_SENSE", STK, NULL, PRIO_ENC, &hENC, 0);
    xTaskCreatePinnedToCore(task_sort_act, "SORT_ACT", STK, NULL, PRIO_SORT, &hSORT, 0);

    // Core 1: Tasks de menor prioridade ou Network
    xTaskCreate(task_monitor, "MONITOR_TASK", STK * 5, (void *)&CURRENT_TEST_MODE, PRIO_MONITOR, NULL);
    xTaskCreate(task_report_60s, "REPORT_60S", STK * 6, (void *)&CURRENT_TEST_MODE, PRIO_MONITOR, NULL);
    xTaskCreate(task_time1, "TIME_TASK", STK * 2, NULL, PRIO_TIME, NULL);

    ESP_LOGI(TAG, "Sistema iniciado.");
} // Fim do app_main