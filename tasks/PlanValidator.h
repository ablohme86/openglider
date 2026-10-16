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
 *  File: PlanValidator.h
 *
 *  Desc:
 *  Declarations and interface for PlanValidator (task graph
 *  definitions, execution, and validation).
 *
 *
 */
#pragma once

#include "og/tasks/Plan.h"

#include <cstddef>
#include <string>
#include <vector>

namespace og
{

struct PlanValidationResult
{
    bool valid = true;
    std::vector<std::string> errors;

    std::string describe() const;
};

class PlanValidator
{
public:
    explicit PlanValidator(std::size_t maxSteps = 50) : maxSteps_(maxSteps)
    {
    }

    PlanValidationResult validate(const Plan& plan) const;

    void setMaxSteps(std::size_t maxSteps)
    {
        maxSteps_ = maxSteps;
    }

    std::size_t maxSteps() const
    {
        return maxSteps_;
    }

private:
    std::size_t maxSteps_ = 50;
};

} // namespace og
