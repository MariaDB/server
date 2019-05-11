/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2012 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include "mrn_match_escalation_threshold_scope.hpp"

namespace mrn {
  MatchEscalationThresholdScope::MatchEscalationThresholdScope(
    grn_ctx *ctx, long long int threshold)
    : ctx_(ctx),
      original_threshold_(grn_ctx_get_match_escalation_threshold(ctx_)) {
    grn_ctx_set_match_escalation_threshold(ctx_, threshold);
  }

  MatchEscalationThresholdScope::~MatchEscalationThresholdScope() {
    grn_ctx_set_match_escalation_threshold(ctx_, original_threshold_);
  }
}
