/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-11
 *  File: ILlmClient.h
 *
 *  Desc:
 *  Declarations and interface for ILlmClient (strategic and tactical AI
 *  planning systems).
 *
 *
 */
#pragma once

#include <string>

namespace og
{

struct LlmRequest
{
    std::string systemPrompt;
    std::string userPrompt;
    double temperature = 0.2;
    int maxTokens = 1500;
};

struct LlmResponse
{
    bool success = false;
    std::string content;
    std::string errorMessage;
};

class ILlmClient
{
public:
    virtual ~ILlmClient() = default;
    virtual LlmResponse complete(const LlmRequest& req) = 0;
};

class MockLlmClient final : public ILlmClient
{
public:
    MockLlmClient() = default;
    explicit MockLlmClient(std::string defaultResponse) : defaultResponse_(std::move(defaultResponse))
    {
    }

    void setResponse(std::string response)
    {
        defaultResponse_ = std::move(response);
    }

    void setFail(bool fail, std::string errorMsg = "Mock failure")
    {
        fail_ = fail;
        errorMsg_ = std::move(errorMsg);
    }

    LlmResponse complete(const LlmRequest& req) override
    {
        lastRequest_ = req;

        if (fail_)
        {
            return LlmResponse{false, "", errorMsg_};
        }

        return LlmResponse{true, defaultResponse_, ""};
    }

    const LlmRequest& lastRequest() const
    {
        return lastRequest_;
    }

private:
    std::string defaultResponse_;
    bool fail_ = false;
    std::string errorMsg_;
    LlmRequest lastRequest_;
};

} // namespace og
