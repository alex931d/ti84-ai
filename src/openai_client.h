#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "camera.h"
#include "config.h"
#include "wifi_manager.h"

namespace OpenAI {

namespace {

constexpr uint16_t kMaxOutputTokens = 512;
constexpr float kRequestTemperature = 0.3f;
constexpr unsigned long kWifiReconnectDelayMs = 250;
constexpr unsigned long kTransportRetryDelayMs = 300;
constexpr unsigned long kWifiReconnectTimeoutMs = 10000;
constexpr uint16_t kHttpConnectTimeoutMs = 15000;
constexpr uint32_t kHttpRequestTimeoutMs = 60000;
constexpr size_t kResponseParseSlack = 4096;
constexpr size_t kErrorParseSlack = 2048;
constexpr size_t kResponseIdDocCapacity = 128;
constexpr char kDefaultVisionPrompt[] = "Read the image and answer.";

String previousResponseId = "";
// Hint-mode conversations are tracked separately from previousResponseId so
// a hint session can never be accidentally continued by a normal Reply (or
// vice versa) -- see OpenAI::hintResponseId below.
String hintResponseId = "";

struct RequestOptions {
    String userMessage;
    String imageBase64;
    String instructions;
    bool isVision = false;
    bool includePreviousResponse = false;
    bool updateConversationState = false;
    // Which of the globals above this request reads/writes conversation
    // continuity against. Always set explicitly by chatWithInstructions().
    String *conversationSlot = nullptr;
};

struct RequestResult {
    int httpCode = HTTPC_ERROR_CONNECTION_REFUSED;
    String rawBody = "";
};

void appendJsonString(String &out, const String &value) {
    out += '"';
    for (unsigned int i = 0; i < value.length(); i++) {
        const char c = value[i];
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
        }
    }
    out += '"';
}

bool shouldStoreResponse(const RequestOptions &options) {
    // No longer excludes vision: hint-mode photo sessions need their first
    // (vision) turn stored so later text-only turns can continue it by id.
    return options.updateConversationState;
}

bool shouldUsePreviousResponse(const RequestOptions &options) {
    return options.includePreviousResponse && options.conversationSlot &&
           options.conversationSlot->length() > 0;
}

String buildVisionInstructions() {
    String instructions = CAMERA_PROMPT;
    if (WifiManager::photoRecapEnabled()) {
        instructions += CAMERA_RECAP_SUFFIX;
    }
    return instructions;
}

void logDnsState(const char *prefix) {
    Serial.printf("%s dns1=%s dns2=%s\n",
                  prefix,
                  WiFi.dnsIP(0).toString().c_str(),
                  WiFi.dnsIP(1).toString().c_str());
}

bool ensureOpenAIHostResolves() {
    IPAddress resolved;
    if (WiFi.hostByName(OPENAI_HOST, resolved)) {
        Serial.printf("DNS OK: %s -> %s\n", OPENAI_HOST, resolved.toString().c_str());
        return true;
    }

    logDnsState("DNS fail:");
    Serial.println("Reconnecting WiFi to recover DNS...");
    WifiManager::disconnect();
    delay(kWifiReconnectDelayMs);
    if (!WifiManager::connect(kWifiReconnectTimeoutMs)) {
        return false;
    }

    if (WiFi.hostByName(OPENAI_HOST, resolved)) {
        Serial.printf("DNS OK after reconnect: %s -> %s\n",
                      OPENAI_HOST, resolved.toString().c_str());
        return true;
    }

    logDnsState("DNS still failing:");
    return false;
}

void addTextInputMessage(JsonArray input, const char *role, const String &text) {
    JsonObject message = input.createNestedObject();
    message["role"] = role;
    JsonArray content = message.createNestedArray("content");
    JsonObject part = content.createNestedObject();
    part["type"] = "input_text";
    part["text"] = text;
}

String buildTextRequestBody(const RequestOptions &options) {
    const size_t slotLen = options.conversationSlot ? options.conversationSlot->length() : 0;
    const size_t capacity =
        1536 + options.instructions.length() + options.userMessage.length() + slotLen;
    DynamicJsonDocument doc(capacity);

    doc["model"] = OPENAI_MODEL;
    doc["instructions"] = options.instructions;
    doc["temperature"] = kRequestTemperature;
    doc["max_output_tokens"] = kMaxOutputTokens;
    doc["store"] = shouldStoreResponse(options);

    if (shouldUsePreviousResponse(options)) {
        doc["previous_response_id"] = *options.conversationSlot;
    }

    JsonArray input = doc.createNestedArray("input");
    addTextInputMessage(input, "user", options.userMessage);

    String body;
    body.reserve(capacity);
    serializeJson(doc, body);
    return body;
}

String buildVisionRequestBody(const RequestOptions &options) {
    // This path is intentionally manual. Fully materializing the image request in
    // ArduinoJson would duplicate a very large base64 string in RAM, which makes
    // TLS uploads noticeably less reliable on the ESP32. The text-only path can
    // use ArduinoJson safely because those payloads stay small.
    const String prompt = options.userMessage.length() > 0
        ? options.userMessage
        : String(kDefaultVisionPrompt);

    String body;
    // Reserve once so we do not keep reallocating while appending a 100KB+ image.
    body.reserve(strlen(OPENAI_MODEL) + options.instructions.length() + prompt.length() +
                 options.imageBase64.length() + 256);
    body += "{\"model\":";
    appendJsonString(body, OPENAI_MODEL);
    body += ",\"instructions\":";
    appendJsonString(body, options.instructions);
    body += ",\"input\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":";
    appendJsonString(body, prompt);
    body += "},{\"type\":\"input_image\",\"image_url\":\"data:image/jpeg;base64,";
    body += options.imageBase64;
    body += "\"}]}],\"max_output_tokens\":";
    body += String(kMaxOutputTokens);
    body += ",\"temperature\":";
    body += String(kRequestTemperature, 1);
    body += ",\"store\":false}";
    return body;
}

String buildRequestBody(const RequestOptions &options) {
    return options.isVision ? buildVisionRequestBody(options)
                            : buildTextRequestBody(options);
}

String extractOutputText(const String &rawResponse) {
    DynamicJsonDocument doc(rawResponse.length() + kResponseParseSlack);
    const DeserializationError err = deserializeJson(doc, rawResponse);
    if (err) {
        return "Error: malformed response";
    }

    JsonArray output = doc["output"].as<JsonArray>();
    if (output.isNull()) {
        return "Error: no content in response";
    }

    String content;
    for (JsonObject item : output) {
        const char *itemType = item["type"] | "";
        if (strcmp(itemType, "message") != 0) {
            continue;
        }

        JsonArray parts = item["content"].as<JsonArray>();
        for (JsonObject part : parts) {
            const char *partType = part["type"] | "";
            const char *text = part["text"] | "";
            if ((strcmp(partType, "output_text") == 0 || strcmp(partType, "text") == 0) &&
                text[0] != '\0') {
                content += text;
            }
        }
    }

    content.trim();
    if (content.endsWith("END OF MESSAGE.")) {
        content = content.substring(0, content.length() - strlen("END OF MESSAGE."));
    } else if (content.endsWith("END OF MESSAGE")) {
        content = content.substring(0, content.length() - strlen("END OF MESSAGE"));
    }
    content.trim();

    return content.length() > 0 ? content : String("Error: no content in response");
}

String extractResponseId(const String &rawResponse) {
    StaticJsonDocument<32> filter;
    filter["id"] = true;

    DynamicJsonDocument doc(kResponseIdDocCapacity);
    const DeserializationError err =
        deserializeJson(doc, rawResponse, DeserializationOption::Filter(filter));
    if (err) {
        return "";
    }

    const char *id = doc["id"] | "";
    return String(id);
}

String extractErrorMessage(const String &rawResponse) {
    DynamicJsonDocument doc(rawResponse.length() + kErrorParseSlack);
    const DeserializationError err = deserializeJson(doc, rawResponse);
    if (err) {
        return rawResponse.substring(0, 200);
    }

    const char *errorMessage = doc["error"]["message"] | nullptr;
    if (errorMessage && errorMessage[0] != '\0') {
        return String(errorMessage);
    }

    const char *message = doc["message"] | nullptr;
    if (message && message[0] != '\0') {
        return String(message);
    }

    return rawResponse.substring(0, 200);
}

RequestResult runRequest(const String &body, const String &apiKey) {
    RequestResult result;

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(30);
    client.setTimeout(30);

    HTTPClient http;
    const String url = "https://" + String(OPENAI_HOST) + String(OPENAI_PATH);
    if (!http.begin(client, url)) {
        result.rawBody = "begin failed";
        return result;
    }

    http.setReuse(false);
    http.setConnectTimeout(kHttpConnectTimeoutMs);
    http.setTimeout(kHttpRequestTimeoutMs);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + apiKey);

    result.httpCode = http.POST(body);
    if (result.httpCode > 0) {
        result.rawBody = http.getString();
    } else {
        result.rawBody = http.errorToString(result.httpCode);
    }
    http.end();

    Serial.printf("API result code: %d\n", result.httpCode);
    return result;
}

RequestResult runRequestWithRetry(const String &body, const String &apiKey) {
    RequestResult result = runRequest(body, apiKey);
    if (result.httpCode > 0) {
        return result;
    }

    Serial.printf("Transport error %d. Retrying once...\n", result.httpCode);
    delay(kTransportRetryDelayMs);
    WifiManager::disconnect();
    delay(kWifiReconnectDelayMs);
    WifiManager::connect(kWifiReconnectTimeoutMs);
    ensureOpenAIHostResolves();
    return runRequest(body, apiKey);
}

// Shared implementation behind chat() and the hint-mode functions. Each
// caller supplies its own system prompt and which conversation-id global to
// read/write, so normal conversations and hint sessions never share state.
String chatWithInstructions(const String &userMessage,
                            const String &imageBase64,
                            bool includeHistory,
                            bool updateConversationState,
                            const String &instructions,
                            String *conversationSlot) {
    if (!WifiManager::isConnected()) {
        return "Error: WiFi not connected";
    }

    const String apiKey = WifiManager::getAPIKey();
    if (apiKey.length() == 0) {
        return "Error: No API key configured";
    }

    RequestOptions options;
    options.userMessage = userMessage;
    options.imageBase64 = imageBase64;
    options.includePreviousResponse = includeHistory;
    options.updateConversationState = updateConversationState;
    options.isVision = imageBase64.length() > 0;
    options.instructions = instructions;
    options.conversationSlot = conversationSlot;

    if (!ensureOpenAIHostResolves()) {
        return "DNS ERROR";
    }

    const String body = buildRequestBody(options);
    RequestResult result = runRequestWithRetry(body, apiKey);

    String response;
    if (result.httpCode == 200) {
        response = extractOutputText(result.rawBody);

        if (shouldStoreResponse(options) && conversationSlot) {
            *conversationSlot = extractResponseId(result.rawBody);
        }
    } else if (result.httpCode > 0) {
        response = "API error " + String(result.httpCode) + ": " +
                   extractErrorMessage(result.rawBody);
        Serial.println(response);
    } else {
        if (result.rawBody == "connection refused") {
            response = "DNS/NET ERROR";
        } else {
            response = "HTTP error " + String(result.httpCode) + ": " + result.rawBody;
        }
        Serial.println(response);
    }

    return response;
}

}  // namespace

void clearConversation() {
    previousResponseId = "";
    Serial.println("Conversation cleared");
}

String conversationResponseId() {
    return previousResponseId;
}

void setConversationResponseId(const String &responseId) {
    previousResponseId = responseId;
}

String chat(const String &userMessage,
            const String &imageBase64 = "",
            bool includeHistory = false,
            bool updateTextHistory = false) {
    const String instructions =
        imageBase64.length() > 0 ? buildVisionInstructions() : String(SYSTEM_PROMPT);
    return chatWithInstructions(userMessage, imageBase64, includeHistory, updateTextHistory,
                                instructions, &previousResponseId);
}

String ask(const String &prompt) {
    clearConversation();
    return chat(prompt, "", false, true);
}

String reply(const String &prompt) {
    return chat(prompt, "", true, true);
}

String solveImage(const String &imageBase64) {
    return chat("", imageBase64, false, false);
}

namespace {
String hintStepInstruction(int step) {
    return "Give hint " + String(step) + " of " + String(HINT_COUNT) +
           ". Do not reveal the final answer.";
}
}  // namespace

String hintAsk(const String &problemText) {
    hintResponseId = "";
    const String message = hintStepInstruction(1) + "\n\n" + problemText;
    return chatWithInstructions(message, "", false, true, HINT_TEXT_PROMPT, &hintResponseId);
}

String hintSnap(const String &imageBase64) {
    hintResponseId = "";
    const String message = hintStepInstruction(1) + " The problem is in this photo.";
    return chatWithInstructions(message, imageBase64, false, true, HINT_VISION_PROMPT, &hintResponseId);
}

String hintNext(int step) {
    return chatWithInstructions(hintStepInstruction(step), "", true, true, HINT_TEXT_PROMPT,
                                &hintResponseId);
}

String hintAnswer() {
    const String message = "Give the complete final answer now, with brief work.";
    return chatWithInstructions(message, "", true, true, HINT_TEXT_PROMPT, &hintResponseId);
}

namespace {

// Runs ask()/reply()/solveImage() on a background task so the main loop keeps
// servicing the calculator link port (cbl.eventLoopTick()) while the HTTP
// request/camera capture is in flight. The task is a self-contained unit of
// work; the only handoff back to the caller's thread is the single result
// string below, guarded by asyncMutex.
SemaphoreHandle_t asyncMutex = xSemaphoreCreateMutex();
bool asyncBusy = false;
bool asyncDone = false;
String asyncResultText;

enum class AsyncKind { Ask, Reply, SolveImage, HintAsk, HintSnap, HintNext, HintAnswer };

struct AsyncTaskParams {
    AsyncKind kind;
    String prompt;
    int hintStep = 0;  // only used by HintNext
};

void asyncTaskEntry(void *paramPtr) {
    AsyncTaskParams *params = static_cast<AsyncTaskParams *>(paramPtr);
    String result;

    switch (params->kind) {
        case AsyncKind::Ask:
            result = ask(params->prompt);
            break;
        case AsyncKind::Reply:
            result = reply(params->prompt);
            break;
        case AsyncKind::SolveImage: {
            const Camera::FocusCheckResult focus = Camera::checkFocusQuality();
            if (!focus.passed) {
                result = "IMAGE UNCLEAR. HOLD STEADY, GET CLOSER, AND RETRY.";
            } else {
                const String imageBase64 = Camera::captureBase64();
                result = imageBase64.length() > 0
                             ? solveImage(imageBase64)
                             : String("Error: camera capture failed");
            }
            break;
        }
        case AsyncKind::HintAsk:
            result = hintAsk(params->prompt);
            break;
        case AsyncKind::HintSnap: {
            const Camera::FocusCheckResult focus = Camera::checkFocusQuality();
            if (!focus.passed) {
                result = "IMAGE UNCLEAR. HOLD STEADY, GET CLOSER, AND RETRY.";
            } else {
                const String imageBase64 = Camera::captureBase64();
                result = imageBase64.length() > 0
                             ? hintSnap(imageBase64)
                             : String("Error: camera capture failed");
            }
            break;
        }
        case AsyncKind::HintNext:
            result = hintNext(params->hintStep);
            break;
        case AsyncKind::HintAnswer:
            result = hintAnswer();
            break;
    }

    delete params;

    Serial.printf("OpenAI task stack high water mark: %u bytes\n",
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    xSemaphoreTake(asyncMutex, portMAX_DELAY);
    asyncResultText = result;
    asyncDone = true;
    xSemaphoreGive(asyncMutex);

    vTaskDelete(nullptr);
}

void beginAsyncTask(AsyncKind kind, const String &prompt, int hintStep = 0) {
    AsyncTaskParams *params = new AsyncTaskParams();
    params->kind = kind;
    params->prompt = prompt;
    params->hintStep = hintStep;

    xSemaphoreTake(asyncMutex, portMAX_DELAY);
    asyncBusy = true;
    asyncDone = false;
    xSemaphoreGive(asyncMutex);

    constexpr uint32_t kTaskStackBytes = 12288;
    constexpr UBaseType_t kTaskPriority = 1;
    constexpr BaseType_t kTaskCore = 0;  // Opposite the Arduino loop task (core 1).
    TaskHandle_t handle = nullptr;
    xTaskCreatePinnedToCore(asyncTaskEntry, "openai_task", kTaskStackBytes, params,
                            kTaskPriority, &handle, kTaskCore);
}

}  // namespace

bool isBusy() {
    xSemaphoreTake(asyncMutex, portMAX_DELAY);
    const bool busy = asyncBusy;
    xSemaphoreGive(asyncMutex);
    return busy;
}

bool pollResult(String *outText) {
    bool ready = false;
    xSemaphoreTake(asyncMutex, portMAX_DELAY);
    if (asyncDone) {
        *outText = asyncResultText;
        asyncResultText = "";
        asyncDone = false;
        asyncBusy = false;
        ready = true;
    }
    xSemaphoreGive(asyncMutex);
    return ready;
}

void beginAsk(const String &prompt) {
    beginAsyncTask(AsyncKind::Ask, prompt);
}

void beginReply(const String &prompt) {
    beginAsyncTask(AsyncKind::Reply, prompt);
}

void beginSolveImage() {
    beginAsyncTask(AsyncKind::SolveImage, "");
}

void beginHintAsk(const String &prompt) {
    beginAsyncTask(AsyncKind::HintAsk, prompt);
}

void beginHintSnap() {
    beginAsyncTask(AsyncKind::HintSnap, "");
}

void beginHintNext(int step) {
    beginAsyncTask(AsyncKind::HintNext, "", step);
}

void beginHintAnswer() {
    beginAsyncTask(AsyncKind::HintAnswer, "");
}

}  // namespace OpenAI
