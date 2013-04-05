/*
 * Copyright 2012 Research In Motion Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <json/writer.h>
#include <stdio.h>
#include <sstream>
#include <string>
#include <map>
#include <algorithm>
#include "push_js.hpp"
#include "push_ndk.hpp"
#include "PipeData.hpp"

namespace webworks {

PushNDK::PushNDK(Push *parent)
    : m_parent(parent), m_pPushService(NULL), m_fileDescriptor(INVALID_PPS_FILE_DESCRIPTOR),
    m_monitorThread(INVALID_THREAD_ID), m_isMonitorThreadRunning(false), m_pConnTimerThread(NULL),
    m_connectionClosePushCommand(INVALID_PUSH_COMMAND), m_hasReceivedConnectionCloseError(false),
    m_wasCreateSessionCalledByUs(false)
{
    pipe(m_pipeFileDescriptors);
}

PushNDK::~PushNDK()
{
    if (m_monitorThread) {
        stopService();
    }

    if (m_pConnTimerThread) {
        delete m_pConnTimerThread;
    }

    if (m_pPushService) {
        delete m_pPushService;
    }
}

void PushNDK::StartService(const std::string& invokeTargetId, const std::string& appId, const std::string& ppgUrl)
{
    if (m_monitorThread) {
        stopService();
    }

    if (m_pConnTimerThread) {
        delete m_pConnTimerThread;
        m_pConnTimerThread = NULL;
    }

    if (m_pPushService) {
        delete m_pPushService;
        m_pPushService = NULL;
    }

    m_fileDescriptor = INVALID_PPS_FILE_DESCRIPTOR;
    m_connectionClosePushCommand = INVALID_PUSH_COMMAND;
    m_hasReceivedConnectionCloseError = false;
    m_wasCreateSessionCalledByUs = false;

    m_invokeTargetId = invokeTargetId;
    m_appId = appId;
    m_ppgUrl = ppgUrl;

    m_pPushService = new PushService(m_appId, m_invokeTargetId);
    m_pConnTimerThread = new ConnectionTimerThread(m_pPushService, m_pipeFileDescriptors[PIPE_WRITE_FD]);

    m_pPushService->setListener(this);

    // Create the Push PPS file descriptor monitor thread
    m_isMonitorThreadRunning = true;
    if (startMonitorThread() == 0) {
        pthread_setname_np(m_monitorThread, "webworks_push_monitor");
        // Successfully started monitor thread
        m_pPushService->createSession();
    } else {
        m_isMonitorThreadRunning = false;
        fprintf(stderr, "StartService: Failed to create monitor thread.\n");
        onCreateSessionComplete(PushStatus(bb::communications::push::PUSH_ERR_INTERNAL_ERROR));
    }
}

void PushNDK::CreateChannel()
{
    if (m_pPushService) {
        m_pPushService->createChannel(m_ppgUrl);
    } else {
        fprintf(stderr, "CreateChannel: Cannot find PushService object.\n");
        onCreateChannelComplete(PushStatus(bb::communications::push::PUSH_ERR_INTERNAL_ERROR), "");
    }
}

void PushNDK::DestroyChannel()
{
    if (m_pPushService) {
        m_pPushService->destroyChannel();
    } else {
        fprintf(stderr, "DestroyChannel: Cannot find PushService object.\n");
        onDestroyChannelComplete(PushStatus(bb::communications::push::PUSH_ERR_INTERNAL_ERROR));
    }
}

std::string PushNDK::ExtractPushPayload(const std::string& invokeData)
{
    std::string decoded_data = decodeBase64(invokeData);
    PushPayload pushPayload(reinterpret_cast<const unsigned char *>(decoded_data.c_str()), decoded_data.length());

    Json::Value payload_obj;

    if (pushPayload.isValid()) {
        payload_obj["valid"] = Json::Value(true);

        // Retrieve the push information
        payload_obj["id"] = Json::Value(pushPayload.getId());
        payload_obj["isAcknowledgeRequired"] = Json::Value(pushPayload.isAckRequired());

        // Retrieve the headers
        Json::Value headers;
        std::map<std::string, std::string> headers_map = pushPayload.getHeaders();
        std::map<std::string, std::string>::const_iterator headers_iter;

        for (headers_iter = headers_map.begin(); headers_iter != headers_map.end(); headers_iter++) {
            headers[headers_iter->first] = Json::Value(headers_iter->second);
        }

        payload_obj["headers"] = headers;

        // Retrieve the data (return as byte array)
        const unsigned char *data = pushPayload.getData();
        Json::UInt current;

        for (int i = 0; i < pushPayload.getDataLength(); i++) {
            current = data[i];
            payload_obj["data"].append(Json::Value(current));
        }
    } else {
        payload_obj["valid"] = Json::Value(false);
    }

    // Write the final JSON object
    Json::FastWriter writer;
    return writer.write(payload_obj);
}

void PushNDK::RegisterToLaunch()
{
    if (m_pPushService) {
        m_pPushService->registerToLaunch();
    } else {
        fprintf(stderr, "RegisterToLaunch: Cannot find PushService object.\n");
        onRegisterToLaunchComplete(PushStatus(bb::communications::push::PUSH_ERR_INTERNAL_ERROR));
    }
}

void PushNDK::UnregisterFromLaunch()
{
    if (m_pPushService) {
        m_pPushService->unregisterFromLaunch();
    } else {
        fprintf(stderr, "UnregisterFromLaunch: Cannot find PushService object.\n");
        onUnregisterFromLaunchComplete(PushStatus(bb::communications::push::PUSH_ERR_INTERNAL_ERROR));
    }
}

void PushNDK::Acknowledge(const std::string& payloadId, bool shouldAccept)
{
    if (m_pPushService) {
        if (shouldAccept) {
            m_pPushService->acceptPush(payloadId);
        } else {
            m_pPushService->rejectPush(payloadId);
        }
    }
}

void PushNDK::onCreateSessionComplete(const PushStatus& status)
{
    // If this flag is set, then we suppress notifying the user of a
    // push.create.callback event since we would have initiated a
    // createSession call in the while loop of the MonitorMessages()
    // function ourselves(so not a user-initiated operation)
    if (m_wasCreateSessionCalledByUs) {
        m_wasCreateSessionCalledByUs = false;
        notifyEventPushServiceConnectionReady();
    } else {
        setConnectionCloseLastPushCommand(status.getCode(), bb::communications::push::CreateSession);

        if (status.getCode() == bb::communications::push::PUSH_ERR_CONNECTION_CLOSE) {
            // The connection timer thread is re-initialized in the
            // StartService function so start it if it is not already
            onConnectionClose();
        }

        std::stringstream ss;
        ss << status.getCode();

        m_parent->NotifyEvent("push.create.callback", ss.str());
    }
}

void PushNDK::onCreateChannelComplete(const PushStatus& status, const std::string& token)
{
    std::stringstream ss;
    ss << status.getCode();
    ss << " ";

    if (status.getCode() == bb::communications::push::PUSH_NO_ERR) {
        ss << token;
        m_parent->NotifyEvent("push.createChannel.callback", ss.str());
    } else {
        setConnectionCloseLastPushCommand(status.getCode(), bb::communications::push::CreateChannel);

        m_parent->NotifyEvent("push.createChannel.callback", ss.str());
    }
}

void PushNDK::onDestroyChannelComplete(const PushStatus& status)
{
    setConnectionCloseLastPushCommand(status.getCode(), bb::communications::push::DestroyChannel);

    std::stringstream ss;
    ss << status.getCode();
    m_parent->NotifyEvent("push.destroyChannel.callback", ss.str());
}

void PushNDK::onRegisterToLaunchComplete(const PushStatus& status)
{
    setConnectionCloseLastPushCommand(status.getCode(), bb::communications::push::RegisterToLaunch);

    std::stringstream ss;
    ss << status.getCode();
    m_parent->NotifyEvent("push.launchApplicationOnPush.callback", ss.str());
}

void PushNDK::onUnregisterFromLaunchComplete(const PushStatus& status)
{
    // Leave this as RegisterToLaunch since, in WebWorks,
    // we have the one launchApplicationOnPush function
    setConnectionCloseLastPushCommand(status.getCode(), bb::communications::push::RegisterToLaunch);

    std::stringstream ss;
    ss << status.getCode();
    m_parent->NotifyEvent("push.launchApplicationOnPush.callback", ss.str());
}

void PushNDK::onSimChange()
{
    m_parent->NotifyEvent("push.create.simChangeCallback", "{}");
}

void PushNDK::onPushTransportReady(PushCommand command)
{
    std::stringstream ss;
    ss << command;
    m_parent->NotifyEvent("push.create.pushTransportReadyCallback", ss.str());
}

void PushNDK::onConnectionClose()
{
    if (m_pConnTimerThread && !m_pConnTimerThread->isRunning()) {
        m_pConnTimerThread->start();
    }
}

void PushNDK::MonitorMessages()
{
    // The pipe is used to send a single byte pipe message to unlock the select request
    char pipe_buf[1];

    while (m_isMonitorThreadRunning)
    {
        // Reset and initialize the file descriptor set
        fd_set fileDescriptorSet;
        int max_fd = 0;
        FD_ZERO(&fileDescriptorSet);

        // Get a new push file descriptor
        m_fileDescriptor = m_pPushService->getPushFd();
        if (m_fileDescriptor == INVALID_PPS_FILE_DESCRIPTOR) {
            fprintf(stderr, "MonitorMessages: Invalid PPS file descriptor.\n");
        }

        // Add PPS file descriptor to the set to monitor in the select
        fdSet(m_fileDescriptor, &max_fd, &fileDescriptorSet);

        // Add pipe file descriptor to the set to monitor in the select
        fdSet(m_pipeFileDescriptors[PIPE_READ_FD], &max_fd, &fileDescriptorSet);

        if ((select(max_fd + 1, &fileDescriptorSet, NULL, NULL, NULL)) > 0) {
            // Check which file descriptor that is being monitored by the select has been changed
            if (FD_ISSET(m_fileDescriptor, &fileDescriptorSet)) {
                m_pPushService->processMsg();
            } else if (FD_ISSET(m_pipeFileDescriptors[PIPE_READ_FD], &fileDescriptorSet )) {
                read(m_pipeFileDescriptors[PIPE_READ_FD], pipe_buf, sizeof(pipe_buf));

                if (pipe_buf[0] == CONNECTION_ESTABLISHED) {
                    // This is called when the onConnectionClose implementation to re-establish
                    // a valid file descriptor has completed (using ConnectionTimerThread)

                    // We set a flag so that when we call createSession on the next
                    // line we can suppress the onCreateSessionComplete callback
                    // from notifying the user of a push.create.callback event
                    m_wasCreateSessionCalledByUs = true;

                    // Create session to reconnect with the PNS Agent
                    m_pPushService->createSession();
                } else {
                    // We are dealing with a STOP_THREAD or PING_SELECT
                    // The STOP_THREAD while cause the while loop to be exited
                    // The PING_SELECT will wake up the select loop to force removal of the invalid file descriptor
                }
            }
        }
    }
}

int PushNDK::startMonitorThread()
{
    return pthread_create(&m_monitorThread, NULL, &MonitorMessagesStartThread, static_cast<void *>(this));
}

void* PushNDK::MonitorMessagesStartThread(void* parent)
{
    PushNDK *pParent = static_cast<PushNDK *>(parent);
    pParent->MonitorMessages();

    return NULL;
}

void PushNDK::stopService()
{
    if (!m_monitorThread) {
        return;
    }

    // Write 1 byte to the pipe to wake up the select which will kick out of the loop by setting the boolean below to false
    m_isMonitorThreadRunning = false;
    char pipeData = STOP_THREAD;
    if (write(m_pipeFileDescriptors[PIPE_WRITE_FD], &pipeData, 1) < 0) {
        fprintf(stderr, "stopService: Failed to write to pipe.\n");
    }

    // Wait for other thread to finish
    pthread_join(m_monitorThread, NULL);
    m_monitorThread = INVALID_THREAD_ID;
}

std::string PushNDK::decodeBase64(const std::string& encodedString)
{
    static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                            "abcdefghijklmnopqrstuvwxyz"
                                            "0123456789+/";

    size_t remaining = encodedString.size();
    size_t position;
    int i = 0, current = 0;
    unsigned char current_set[4];
    std::string decoded_string;

    while ((remaining--) && ((position = base64_chars.find(encodedString[current++])) != std::string::npos)) {
        current_set[i++] = static_cast<unsigned char>(position);

        if (i == 4) {
            i = 0;
            decoded_string += (current_set[0] << 2) | ((current_set[1] & 0x30) >> 4);
            decoded_string += ((current_set[1] & 0xf) << 4) | ((current_set[2] & 0x3c) >> 2);
            decoded_string += ((current_set[2] & 0x3) << 6) | current_set[3];
        }
    }

    if (i) {
        if (i >= 2) {
            decoded_string += (current_set[0] << 2) | ((current_set[1] & 0x30) >> 4);
        }

        if (i >= 3) {
            decoded_string += ((current_set[1] & 0xf) << 4) | ((current_set[2] & 0x3c) >> 2);
        }
    }

    return decoded_string;
}

void PushNDK::fdSet(int fd, int* maxFd, fd_set* rfds)
{
    if (fd > 0 && fd < FD_SETSIZE) {
        if (fd > *maxFd) {
            *maxFd = fd;
        }

        FD_SET(fd, rfds);
    }
}

void PushNDK::notifyEventPushServiceConnectionReady()
{
    if (m_hasReceivedConnectionCloseError) {
        m_hasReceivedConnectionCloseError = false;

        std::stringstream ss;
        ss << m_connectionClosePushCommand;
        m_parent->NotifyEvent("push.create.pushServiceConnectionReadyCallback", ss.str());
    }
}

void PushNDK::setConnectionCloseLastPushCommand(int code, PushCommand command)
{
    if (code == bb::communications::push::PUSH_ERR_CONNECTION_CLOSE) {
        m_connectionClosePushCommand = command;
        m_hasReceivedConnectionCloseError = true;
    }
}

} // namespace webworks
