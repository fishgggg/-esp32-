/**
 * @file    app_web.c
 * @brief   SmartHome Web 仪表盘 — HTTP 服务器 + SSE + REST API (S7)
 */

#include "app_web.h"
#include "app_camera.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>

static const char *TAG = "web";

/* ================================================================
 * 全局状态
 * ================================================================ */

static QueueHandle_t g_sensor_q = NULL;    /* 本地缓存的 sensor_queue */
static QueueHandle_t g_cmd_q    = NULL;    /* 本地缓存的 cmd_queue    */
static SensorData    g_last_sensor = {0};  /* 最新传感器数据缓存 */

/* 照片缓冲 */
static uint8_t *g_photo_buf = NULL;
static size_t   g_photo_len = 0;

/* SSE 客户端链表 */
typedef struct sse_client {
    int                    fd;
    httpd_req_t           *async_req;  /* async handler 句柄, cleanup 时需要 */
    TickType_t             last_ping;  /* 上次心跳时间 */
    struct sse_client     *next;
} sse_client_t;

static sse_client_t *g_sse_clients = NULL;

/* ================================================================
 * 嵌入式 HTML 仪表盘页面
 * ================================================================ */
static const char INDEX_HTML[] =
"<!DOCTYPE html>"
"<html lang=\"zh\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>SmartHome 控制面板</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
"background:#0d1117;color:#c9d1d9;min-height:100vh;padding:16px}"
"h1{text-align:center;font-size:1.4em;margin-bottom:4px;color:#58a6ff}"
".subtitle{text-align:center;font-size:0.8em;color:#8b949e;margin-bottom:16px}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin-bottom:16px}"
".card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;text-align:center}"
".card .icon{font-size:1.8em;margin-bottom:4px}"
".card .label{font-size:0.7em;color:#8b949e;text-transform:uppercase}"
".card .value{font-size:1.3em;font-weight:bold;color:#e6edf3;margin-top:2px}"
".card .unit{font-size:0.6em;color:#8b949e}"
".card.pir-active{border-color:#3fb950;box-shadow:0 0 8px rgba(63,185,80,0.3)}"
".card.pir-idle{border-color:#30363d}"
"h2{font-size:1em;color:#58a6ff;margin:12px 0 8px;border-bottom:1px solid #30363d;padding-bottom:4px}"
".voice-bar{display:flex;align-items:center;gap:8px;margin-bottom:8px}"
".btn-mic{flex:0 0 auto;min-width:60px;padding:12px;border:none;border-radius:50px;"
"background:#238636;color:#fff;font-size:1.5em;cursor:pointer;transition:all 0.2s}"
".btn-mic:hover{background:#2ea043}"
".btn-mic.listening{background:#f85149;animation:pulse 1.2s infinite}"
"@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.5}}"
".voice-text{flex:1;font-size:0.85em;color:#8b949e;min-height:1.2em;text-align:left}"
".voice-text.cmd{color:#58a6ff;font-weight:bold}"
".voice-text.err{color:#f85149}"
".controls{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:16px}"
".btn{flex:1;min-width:90px;padding:10px 6px;border:none;border-radius:6px;font-size:0.85em;"
"cursor:pointer;transition:all 0.2s;font-weight:bold}"
".btn:active{transform:scale(0.95)}"
".btn-buzzer{background:#b62324;color:#fff}"
".btn-buzzer.on{background:#3fb950}"
".btn-light{background:#444;color:#888}"
".btn-light.on{background:#f0c000;color:#000}"
".btn-curtain{background:#444;color:#888}"
".btn-curtain.on{background:#1f6feb;color:#fff}"
".btn-ac{background:#444;color:#888}"
".btn-ac.on{background:#3fb950;color:#fff}"
".btn-camera{background:#7c3aed;color:#fff}"
".device-leds{display:flex;gap:10px;justify-content:center;margin-bottom:8px;font-size:0.7em;color:#8b949e}"
".device-leds span{display:flex;align-items:center;gap:3px}"
".led-dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#444}"
".led-dot.on{background:#3fb950;box-shadow:0 0 6px #3fb950}"
".led-dot.off{background:#444}"
".photo-area{background:#161b22;border:1px solid #30363d;border-radius:8px;"
"padding:8px;text-align:center;min-height:100px;display:flex;"
"align-items:center;justify-content:center}"
".photo-area img{max-width:100%;max-height:300px;border-radius:4px}"
".photo-area .placeholder{color:#484f58;font-size:0.9em}"
".status{text-align:center;font-size:0.7em;color:#8b949e;margin-top:12px}"
"#dot{color:#3fb950;font-size:1.2em}"
"#dot.off{color:#f85149}"
"#sse-debug{font-size:0.7em;color:#d2991d;text-align:center;margin-top:4px;min-height:1.2em}"
"</style>"
"</head>"
"<body>"
"<h1>SmartHome 智能家居</h1>"
"<p class=\"subtitle\">语音控制 · 实时监控</p>"
"<div class=\"grid\">"
"<div class=\"card\"><div class=\"icon\">&#127777;</div><div class=\"label\">温度</div>"
"<div class=\"value\" id=\"temp\">--</div><div class=\"unit\">&deg;C</div></div>"
"<div class=\"card\"><div class=\"icon\">&#128167;</div><div class=\"label\">湿度</div>"
"<div class=\"value\" id=\"humi\">--</div><div class=\"unit\">%</div></div>"
"<div class=\"card\"><div class=\"icon\">&#9728;</div><div class=\"label\">光照</div>"
"<div class=\"value\" id=\"lux\">--</div><div class=\"unit\">lx</div></div>"
"<div class=\"card\"><div class=\"icon\">&#128168;</div><div class=\"label\">CO2</div>"
"<div class=\"value\" id=\"co2\">--</div><div class=\"unit\">ppm</div></div>"
"<div class=\"card\"><div class=\"icon\">&#128168;</div><div class=\"label\">PM2.5</div>"
"<div class=\"value\" id=\"pm25\">--</div><div class=\"unit\">idx</div></div>"
"<div class=\"card pir-idle\" id=\"pir-card\"><div class=\"icon\">&#128065;</div><div class=\"label\">人体检测</div>"
"<div class=\"value\" id=\"pir\">无人</div></div>"
"</div>"
"<h2>&#127908; 语音/文字控制</h2>"
"<div class=\"voice-bar\">"
"<button class=\"btn-mic\" id=\"btn-mic\""
" onpointerdown=\"startVoice()\" onpointerup=\"stopVoice()\" onpointerleave=\"stopVoice()\""
" ontouchstart=\"startVoice()\" ontouchend=\"stopVoice()\">&#127908;</button>"
"<div class=\"voice-text\" id=\"voice-text\">长按说话, 或下方输入命令</div>"
"</div>"
"<div class=\"voice-bar\" style=\"margin-bottom:8px\">"
"<input type=\"text\" id=\"text-cmd\" placeholder=\"输入命令, 如「开灯」「关空调」\""
" style=\"flex:1;padding:10px;border:1px solid #30363d;border-radius:6px;"
"background:#161b22;color:#c9d1d9;font-size:0.9em;outline:none\""
" onkeydown=\"if(event.key==='Enter')sendText()\">"
"<button class=\"btn btn-mic\" style=\"min-width:50px;font-size:1em;background:#1f6feb\""
" onclick=\"sendText()\">发送</button>"
"</div>"
"<div class=\"device-leds\">"
"<span><i class=\"led-dot off\" id=\"led-buzzer\"></i>蜂鸣器</span>"
"<span><i class=\"led-dot off\" id=\"led-light\"></i>灯</span>"
"<span><i class=\"led-dot off\" id=\"led-curtain\"></i>窗帘</span>"
"<span><i class=\"led-dot off\" id=\"led-ac\"></i>空调</span>"
"</div>"
"<h2>&#127918; 设备控制</h2>"
"<div class=\"controls\">"
"<button class=\"btn btn-buzzer\" id=\"btn-buzzer\" onclick=\"toggleBuzzer()\">&#128276; 蜂鸣器(关)</button>"
"<button class=\"btn btn-light\" id=\"btn-light\" onclick=\"toggleLight()\">&#128161; 灯(关)</button>"
"<button class=\"btn btn-curtain\" id=\"btn-curtain\" onclick=\"toggleCurtain()\">&#127695; 窗帘(关)</button>"
"<button class=\"btn btn-ac\" id=\"btn-ac\" onclick=\"toggleAc()\">&#128261; 空调(关)</button>"
"<button class=\"btn btn-camera\" onclick=\"capturePhoto()\">&#128247; 拍照</button>"
"</div>"
"<h2>&#128247; 安防照片</h2>"
"<div class=\"photo-area\" id=\"photo-area\">"
"<span class=\"placeholder\">暂无照片 &mdash; 点击拍照按钮</span>"
"</div>"
"<p class=\"status\"><span id=\"dot\">&bull;</span> 已连接</p>"
"<p id=\"sse-debug\">SSE: 等待数据...</p>"
"<script>"
"/* ==== S8: 设备状态 (来自 SSE 硬件真相, 非本地猜测) ==== */"
"var devState={buzzer:false,light:false,curtain:false,ac:false};"
"var sseDebug=document.getElementById('sse-debug');"
"var voiceText=document.getElementById('voice-text');"
"var micBtn=document.getElementById('btn-mic');"
"/* ==== SSE 事件流 ==== */"
"var evtSource=new EventSource('/events');"
"evtSource.onopen=function(){"
"sseDebug.textContent='SSE: 已连接, 等待数据...';"
"sseDebug.style.color='#d2991d';"
"};"
"evtSource.onmessage=function(e){"
"try{"
"var d=JSON.parse(e.data);"
"sseDebug.textContent='SSE: 数据正常 ['+new Date().toLocaleTimeString()+']';"
"sseDebug.style.color='#3fb950';"
"document.getElementById('temp').textContent=d.temp.toFixed(1);"
"document.getElementById('humi').textContent=d.humi.toFixed(1);"
"document.getElementById('lux').textContent=d.lux;"
"document.getElementById('co2').textContent=d.co2;"
"document.getElementById('pm25').textContent=d.pm25;"
"var pirEl=document.getElementById('pir');"
"var pirCard=document.getElementById('pir-card');"
"if(d.pir){pirEl.textContent='有人';pirCard.className='card pir-active';}"
"else{pirEl.textContent='无人';pirCard.className='card pir-idle';}"
"document.getElementById('dot').className='';"
"/* S8: 同步设备状态 (从硬件真值) */"
"devState.buzzer=d.buzzer;devState.light=d.light;"
"devState.curtain=d.curtain;devState.ac=d.ac;"
"updateAllButtons();"
"}catch(_){}"
"};"
"evtSource.onerror=function(e){"
"document.getElementById('dot').className='off';"
"sseDebug.textContent='SSE: 连接错误, 将自动重连...';"
"sseDebug.style.color='#f85149';"
"};"
"/* ==== S8: 更新按钮 + LED 指示 (SSE 推送后调用) ==== */"
"function updateAllButtons(){"
"var b;"
"b=document.getElementById('btn-buzzer');"
"b.className=devState.buzzer?'btn btn-buzzer on':'btn btn-buzzer';"
"b.textContent=devState.buzzer?'\\uD83D\\uDD14 蜂鸣器(开)':'\\uD83D\\uDD15 蜂鸣器(关)';"
"b=document.getElementById('btn-light');"
"b.className=devState.light?'btn btn-light on':'btn btn-light';"
"b.textContent=devState.light?'\\uD83D\\uDCA1 灯(开)':'\\uD83D\\uDCA1 灯(关)';"
"b=document.getElementById('btn-curtain');"
"b.className=devState.curtain?'btn btn-curtain on':'btn btn-curtain';"
"b.textContent=devState.curtain?'\\uD83D\\uDF96 窗帘(开)':'\\uD83C\\uDF20 窗帘(关)';"
"b=document.getElementById('btn-ac');"
"b.className=devState.ac?'btn btn-ac on':'btn btn-ac';"
"b.textContent=devState.ac?'\\uD83D\\uDD25 空调(开)':'\\uD83D\\uDD0C 空调(关)';"
"document.getElementById('led-buzzer').className=devState.buzzer?'led-dot on':'led-dot off';"
"document.getElementById('led-light').className=devState.light?'led-dot on':'led-dot off';"
"document.getElementById('led-curtain').className=devState.curtain?'led-dot on':'led-dot off';"
"document.getElementById('led-ac').className=devState.ac?'led-dot on':'led-dot off';"
"}"
"/* ==== REST API 发送 ==== */"
"function sendCmd(cmd){"
"fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify(cmd)}).then(function(r){return r.json()}).then(function(j){console.log(j);})"
".catch(function(e){console.error(e);});"
"}"
"/* ==== 手动按钮 (toggle 基于 SSE 硬件状态) ==== */"
"function toggleBuzzer(){sendCmd({cmd:'buzzer',value:!devState.buzzer});}"
"function toggleLight(){sendCmd({cmd:'light',value:!devState.light});}"
"function toggleCurtain(){sendCmd({cmd:'curtain',value:!devState.curtain?'open':'close'});}"
"function toggleAc(){sendCmd({cmd:'ac',value:!devState.ac});}"
"function capturePhoto(){sendCmd({cmd:'camera'});setTimeout(refreshPhoto,3000);}"
"function refreshPhoto(){"
"var area=document.getElementById('photo-area');"
"area.innerHTML='<img src=\"/photo.jpg?t='+Date.now()+'\">';"
"}"
"/* ==== S8: Web Speech API 语音识别 ==== */"
"var SpeechRecognition=window.SpeechRecognition||window.webkitSpeechRecognition;"
"var recognition=null;"
"var listening=false;"
"/* 关键词命令映射: 所有关键词必须同时命中 */"
"var CMD_MAP=["
"{kw:['蜂鸣','开'],   cmd:'buzzer', val:true,  label:'蜂鸣器 开'},"
"{kw:['蜂鸣','关'],   cmd:'buzzer', val:false, label:'蜂鸣器 关'},"
"{kw:['蜂鸣','停'],   cmd:'buzzer', val:false, label:'蜂鸣器 关'},"
"{kw:['灯','开'],     cmd:'light',  val:true,  label:'灯 开'},"
"{kw:['灯','关'],     cmd:'light',  val:false, label:'灯 关'},"
"{kw:['窗帘','开'],   cmd:'curtain',val:'open',label:'窗帘 开'},"
"{kw:['窗帘','关'],   cmd:'curtain',val:'close',label:'窗帘 关'},"
"{kw:['空调','开'],   cmd:'ac',     val:true,  label:'空调 开'},"
"{kw:['空调','关'],   cmd:'ac',     val:false, label:'空调 关'},"
"];"
"function startVoice(){"
"if(!SpeechRecognition){"
"voiceText.textContent='\\u26A0 请用 Chrome 浏览器打开此页面';"
"voiceText.className='voice-text err';"
"return;"
"}"
"if(listening) return;"
"if(!recognition){"
"recognition=new SpeechRecognition();"
"recognition.lang='zh-CN';"
"recognition.interimResults=false;"
"recognition.maxAlternatives=1;"
"recognition.continuous=false;"
"recognition.onresult=function(e){"
"listening=false;"
"micBtn.classList.remove('listening');"
"var said=e.results[0][0].transcript.trim();"
"voiceText.textContent='\\uD83C\\uDF99 听到: \\u300C'+said+'\\u300D';"
"voiceText.className='voice-text';"
"var matched=matchCommand(said);"
"if(matched){"
"voiceText.textContent='\\u2705 '+matched.label;"
"voiceText.className='voice-text cmd';"
"sendCmd({cmd:matched.cmd,value:matched.val});"
"}else{"
"voiceText.textContent='\\u2753 未匹配: \\u300C'+said+'\\u300D';"
"voiceText.className='voice-text err';"
"}"
"};"
"recognition.onerror=function(e){"
"listening=false;"
"micBtn.classList.remove('listening');"
"if(e.error==='network'){"
"voiceText.textContent='\\u26A0 网络错误 — 语音识别需要联网, 请确保移动数据已开启';"
"}else if(e.error==='not-allowed'){"
"voiceText.textContent='\\u26A0 麦克风权限未授权, 请在浏览器设置中允许';"
"}else if(e.error==='no-speech'){"
"voiceText.textContent='\\u26A0 未检测到语音, 请长按后说话';"
"}else{"
"voiceText.textContent='\\u26A0 识别失败 ['+e.error+'] 请重试';"
"}"
"voiceText.className='voice-text err';"
"};"
"recognition.onend=function(){"
"listening=false;"
"micBtn.classList.remove('listening');"
"};"
"}"
"listening=true;"
"micBtn.classList.add('listening');"
"voiceText.textContent='\\uD83C\\uDF99 正在听... 松手结束';"
"voiceText.className='voice-text';"
"try{recognition.start();}catch(e){"
"listening=false;"
"micBtn.classList.remove('listening');"
"voiceText.textContent='\\u26A0 启动失败: '+e.message;"
"voiceText.className='voice-text err';"
"}"
"}"
"function stopVoice(){"
"if(listening&&recognition){"
"try{recognition.stop();}catch(e){}"
"}"
"}"
"/* ==== 文字输入命令 ==== */"
"function sendText(){"
"var inp=document.getElementById('text-cmd');"
"var said=inp.value.trim();"
"if(!said) return;"
"inp.value='';"
"var matched=matchCommand(said);"
"if(matched){"
"voiceText.textContent='\\u2705 '+matched.label+' \\u2192 已发送';"
"voiceText.className='voice-text cmd';"
"sendCmd({cmd:matched.cmd,value:matched.val});"
"}else{"
"voiceText.textContent='\\u2753 未匹配: \\u300C'+said+'\\u300D 试试「开灯」「关空调」';"
"voiceText.className='voice-text err';"
"}"
"}"
"function matchCommand(said){"
"var best=null, bestScore=0;"
"for(var i=0;i<CMD_MAP.length;i++){"
"var c=CMD_MAP[i], score=0;"
"for(var j=0;j<c.kw.length;j++){"
"if(said.indexOf(c.kw[j])>=0) score++;"
"}"
"if(score===c.kw.length&&score>bestScore){"
"best=c;bestScore=score;"
"}"
"}"
"return best;"
"}"
"</script>"
"</body></html>";

/* ================================================================
 * SSE 广播: 向所有已连接的 SSE 客户端推送数据
 * ================================================================ */
static void sse_broadcast(const char *data)
{
    sse_client_t *c = g_sse_clients;
    sse_client_t *prev = NULL;

    while (c) {
        /* 构造 SSE 帧: "data: {...}\n\n" */
        char buf[512];
        int len = snprintf(buf, sizeof(buf), "data: %s\n\n", data);

        /* 非阻塞 TCP 发送 */
        int sent = send(c->fd, buf, len, 0);
        if (sent <= 0) {
            /* 客户端已断开, 从链表中移除 */
            ESP_LOGI(TAG, "SSE client fd=%d disconnected", c->fd);
            /* 标记 async handler 完成, 释放资源 */
            if (c->async_req) {
                httpd_req_async_handler_complete(c->async_req);
            }
            if (prev) {
                prev->next = c->next;
            } else {
                g_sse_clients = c->next;
            }
            sse_client_t *old = c;
            c = c->next;
            free(old);
            continue;
        }
        c->last_ping = xTaskGetTickCount();  /* 更新最后活跃时间 */
        prev = c;
        c = c->next;
    }
}

/* ================================================================
 * SSE 心跳: 向所有已连接的 SSE 客户端发送心跳, 检测断开
 * ================================================================ */
static void sse_heartbeat(void)
{
    sse_client_t *c = g_sse_clients;
    sse_client_t *prev = NULL;
    TickType_t now = xTaskGetTickCount();

    while (c) {
        /* 每 10 秒发送一次心跳 ping */
        if ((now - c->last_ping) >= pdMS_TO_TICKS(10000)) {
            if (send(c->fd, ":ping\n\n", 7, 0) <= 0) {
                /* 客户端已断开 */
                ESP_LOGI(TAG, "SSE client fd=%d disconnected (heartbeat)", c->fd);
                if (c->async_req) {
                    httpd_req_async_handler_complete(c->async_req);
                }
                if (prev) {
                    prev->next = c->next;
                } else {
                    g_sse_clients = c->next;
                }
                sse_client_t *old = c;
                c = c->next;
                free(old);
                continue;
            }
            c->last_ping = now;
        }
        prev = c;
        c = c->next;
    }
}

/* ================================================================
 * HTTP 处理器: GET / — 返回仪表盘 HTML
 * ================================================================ */
static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
    return ESP_OK;
}

/* ================================================================
 * HTTP 处理器: GET /events — SSE 事件流
 *
 * 关键: 不使用 httpd_resp_send_chunk(), 它会自动加 Transfer-Encoding: chunked,
 * 但后续 sse_broadcast() 用 raw send() 发包, 格式不一致导致浏览器断开连接。
 * 改为全程 raw send(), 保持 HTTP 响应层一致性。
 * ================================================================ */
static esp_err_t handler_events(httpd_req_t *req)
{
    /* 获取 socket fd */
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        ESP_LOGE(TAG, "SSE: cannot get socket fd");
        return ESP_FAIL;
    }

    /* 手动发送 HTTP 响应头 (无 chunked encoding) */
    const char *resp_headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    if (send(fd, resp_headers, strlen(resp_headers), 0) < 0) {
        ESP_LOGE(TAG, "SSE: header send failed");
        return ESP_FAIL;
    }

    /* 发送初始 SSE 握手 */
    if (send(fd, ":\n\n", 3, 0) < 0) {
        ESP_LOGE(TAG, "SSE handshake send failed");
        return ESP_FAIL;
    }

    /* 转为异步处理 — handler 可以返回, HTTP 服务器继续处理其他请求 */
    httpd_req_t *async_req = NULL;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        ESP_LOGE(TAG, "SSE: async handler begin failed");
        return ESP_FAIL;
    }

    /* 注册此客户端 */
    sse_client_t *client = calloc(1, sizeof(sse_client_t));
    if (client == NULL) {
        httpd_req_async_handler_complete(async_req);
        return ESP_FAIL;
    }
    client->fd        = fd;
    client->async_req = async_req;
    client->last_ping = xTaskGetTickCount();
    client->next      = g_sse_clients;
    g_sse_clients     = client;

    ESP_LOGI(TAG, "SSE client connected (fd=%d)", fd);

    /* 立即返回 — HTTP 服务器线程可处理其他请求 */
    return ESP_OK;
}

/* ================================================================
 * HTTP 处理器: POST /api/control — 接收控制命令
 * ================================================================ */
static esp_err_t handler_control(httpd_req_t *req)
{
    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    ControlCmd cmd = {0};
    cJSON *cmd_str = cJSON_GetObjectItem(json, "cmd");
    cJSON *val     = cJSON_GetObjectItem(json, "value");

    if (cmd_str && cmd_str->valuestring) {
        if (strcmp(cmd_str->valuestring, "buzzer") == 0) {
            cmd.type = CMD_BUZZER;
            cmd.value = (val && cJSON_IsTrue(val)) ? 1 : 0;
        } else if (strcmp(cmd_str->valuestring, "light") == 0) {
            cmd.type = CMD_RELAY;
            cmd.id   = 0;    /* relay1 */
            cmd.value = (val && cJSON_IsTrue(val)) ? 1 : 0;
        } else if (strcmp(cmd_str->valuestring, "curtain") == 0) {
            cmd.type = CMD_CURTAIN;
            cmd.value = (val && val->valuestring && strcmp(val->valuestring, "open") == 0) ? 1 : 0;
        } else if (strcmp(cmd_str->valuestring, "ac") == 0) {
            cmd.type = CMD_RELAY;
            cmd.id   = 1;    /* AC — GPIO25 LED */
            cmd.value = (val && cJSON_IsTrue(val)) ? 1 : 0;
        } else if (strcmp(cmd_str->valuestring, "camera") == 0) {
            cmd.type = CMD_BUZZER;  /* 复用, 特殊处理 */
            cmd.value = 99;          /* 拍照指令标志 */
        }
    }
    cJSON_Delete(json);

    if (g_cmd_q) {
        xQueueSend(g_cmd_q, &cmd, 0);  /* 非阻塞 */
    }

    const char *resp = "{\"status\":\"ok\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* ================================================================
 * HTTP 处理器: GET /photo.jpg — 返回最新照片
 * ================================================================ */
static esp_err_t handler_photo(httpd_req_t *req)
{
    if (g_photo_buf == NULL || g_photo_len == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No photo");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_send(req, (const char *)g_photo_buf, (int)g_photo_len);
    return ESP_OK;
}

/* ================================================================
 * URI 注册
 * ================================================================ */
static void register_handlers(httpd_handle_t server)
{
    httpd_uri_t uri = {0};

    /* GET / */
    uri.uri     = "/";
    uri.method  = HTTP_GET;
    uri.handler = handler_index;
    httpd_register_uri_handler(server, &uri);

    /* GET /events (SSE) */
    uri.uri     = "/events";
    uri.method  = HTTP_GET;
    uri.handler = handler_events;
    httpd_register_uri_handler(server, &uri);

    /* POST /api/control */
    uri.uri     = "/api/control";
    uri.method  = HTTP_POST;
    uri.handler = handler_control;
    httpd_register_uri_handler(server, &uri);

    /* GET /photo.jpg */
    uri.uri     = "/photo.jpg";
    uri.method  = HTTP_GET;
    uri.handler = handler_photo;
    httpd_register_uri_handler(server, &uri);
}

/* ================================================================
 * 拍照处理 (由 cmd_queue 消费时调用)
 * ================================================================ */
void web_set_photo(const uint8_t *jpeg, size_t len)
{
    if (g_photo_buf) {
        free(g_photo_buf);
        g_photo_buf = NULL;
    }
    g_photo_len = 0;

    if (jpeg && len > 0) {
        g_photo_buf = malloc(len);
        if (g_photo_buf) {
            memcpy(g_photo_buf, jpeg, len);
            g_photo_len = len;
            ESP_LOGI(TAG, "Photo stored: %zu bytes", len);
        }
    }
}

/* ================================================================
 * HttpTask 主循环
 * ================================================================ */
void StartHttpTask(void *pvParameters)
{
    HttpTaskParams *params = (HttpTaskParams *)pvParameters;
    g_sensor_q = params ? params->sensor_queue : NULL;
    g_cmd_q    = params ? params->cmd_queue    : NULL;

    ESP_LOGI(TAG, "HttpTask starting on Core %d", xPortGetCoreID());

    /* ---- 启动 HTTP 服务器 ---- */
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        vTaskDelete(NULL);
        return;
    }

    register_handlers(server);
    ESP_LOGI(TAG, "HTTP server started on port %d", HTTP_PORT);
    ESP_LOGI(TAG, "Dashboard: http://192.168.4.1/");

    /* ---- 初始化 ESP32-CAM UART ---- */
    esp_err_t cam_ret = Camera_Init();
    if (cam_ret != ESP_OK) {
        ESP_LOGW(TAG, "Camera init failed — photo capture disabled");
    }

    /* ---- S8: 初始化设备状态 LED 引脚 ---- */
    gpio_set_direction(LIGHT_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(CURTAIN_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(AC_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LIGHT_LED_GPIO, 0);     /* 初始: 全关 */
    gpio_set_level(CURTAIN_LED_GPIO, 0);
    gpio_set_level(AC_LED_GPIO, 0);
    ESP_LOGI(TAG, "Device LEDs ready (Light=GPIO%d, Curtain=GPIO%d, AC=GPIO%d)",
             LIGHT_LED_GPIO, CURTAIN_LED_GPIO, AC_LED_GPIO);

    /* ---- 主循环: SSE 推送 + 命令处理 ---- */
    TickType_t last_heartbeat = xTaskGetTickCount();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));

        /* -- SSE 推送最新传感器数据 -- */
        if (g_sensor_q && g_sse_clients) {
            /* 排空队列, 只保留最新数据 */
            SensorData latest;
            bool has_data = false;
            while (xQueueReceive(g_sensor_q, &latest, 0) == pdTRUE) {
                g_last_sensor = latest;
                has_data = true;
            }
            if (!has_data) {
                latest = g_last_sensor;  /* 无新数据, 使用缓存 */
            }
            if (g_last_sensor.temperature > -100) {  /* 有有效数据 */
                /* S8: 设备状态 — 全部从 g_device_state 读取 (由 SensorTask+HttpTask+MQTT 三方维护) */
                char json[384];
                snprintf(json, sizeof(json),
                    "{\"temp\":%.1f,\"humi\":%.1f,\"lux\":%u,"
                    "\"co2\":%u,\"pm25\":%u,\"pir\":%s,"
                    "\"buzzer\":%s,\"light\":%s,\"curtain\":%s,\"ac\":%s}",
                    g_last_sensor.temperature,
                    g_last_sensor.humidity,
                    g_last_sensor.light,
                    g_last_sensor.co2,
                    g_last_sensor.pm25,
                    g_last_sensor.motion ? "true" : "false",
                    g_device_state.buzzer ? "true" : "false",
                    g_device_state.light ? "true" : "false",
                    g_device_state.curtain ? "true" : "false",
                    g_device_state.ac ? "true" : "false");
                sse_broadcast(json);
            }
        }

        /* -- SSE 心跳检测 (每 ~10s) -- */
        if ((xTaskGetTickCount() - last_heartbeat) >= pdMS_TO_TICKS(10000)) {
            sse_heartbeat();
            last_heartbeat = xTaskGetTickCount();
        }

        /* -- 处理控制命令 (S8: LED + 打盹) -- */
        if (g_cmd_q) {
            ControlCmd cmd;
            while (xQueueReceive(g_cmd_q, &cmd, 0) == pdTRUE) {
                if (cmd.type == CMD_BUZZER) {
                    if (cmd.value == 99) {
                        /* 拍照指令 */
                        ESP_LOGI(TAG, "Camera capture requested from web");
                        uint8_t *jpeg = NULL;
                        size_t jpeg_len = 0;
                        if (Camera_Capture(&jpeg, &jpeg_len) == ESP_OK) {
                            web_set_photo(jpeg, jpeg_len);
                            Camera_Free();
                        }
                    } else if (cmd.value == 0) {
                        /* 关闭蜂鸣器 + 启动 3s 打盹 (防止立即被 SensorTask 重新触发) */
                        gpio_set_level(BUZZER_GPIO, 1);
                        g_device_state.buzzer = false;
                        g_device_state.buzzer_snooze_until = (int64_t)xTaskGetTickCount()
                                                           + pdMS_TO_TICKS(BUZZER_SNOOZE_MS);
                        ESP_LOGI(TAG, "Buzzer: OFF + snooze 3s (web)");
                    } else {
                        gpio_set_level(BUZZER_GPIO, 0);
                        g_device_state.buzzer = true;
                        ESP_LOGI(TAG, "Buzzer: ON (web)");
                    }
                } else if (cmd.type == CMD_RELAY) {
                    if (cmd.id == 0) {
                        /* 灯 — GPIO32 LED */
                        gpio_set_level(LIGHT_LED_GPIO, cmd.value ? 1 : 0);
                        g_device_state.light = (cmd.value != 0);
                        ESP_LOGI(TAG, "Light: %s (web)", g_device_state.light ? "ON" : "OFF");
                    } else if (cmd.id == 1) {
                        /* 空调 — GPIO25 LED */
                        gpio_set_level(AC_LED_GPIO, cmd.value ? 1 : 0);
                        g_device_state.ac = (cmd.value != 0);
                        ESP_LOGI(TAG, "AC: %s (web)", g_device_state.ac ? "ON" : "OFF");
                    } else {
                        ESP_LOGI(TAG, "Relay%d: %d (HW not connected)", cmd.id, cmd.value);
                    }
                } else if (cmd.type == CMD_CURTAIN) {
                    /* 窗帘 — GPIO33 LED */
                    gpio_set_level(CURTAIN_LED_GPIO, cmd.value ? 1 : 0);
                    g_device_state.curtain = (cmd.value != 0);
                    ESP_LOGI(TAG, "Curtain: %s (web)", g_device_state.curtain ? "OPEN" : "CLOSE");
                } else {
                    ESP_LOGI(TAG, "Cmd: type=%d id=%d val=%d (no HW)", cmd.type, cmd.id, cmd.value);
                }
            }
        }
    }
}
