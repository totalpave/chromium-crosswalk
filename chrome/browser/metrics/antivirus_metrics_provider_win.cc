// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/metrics/antivirus_metrics_provider_win.h"

#include "base/bind.h"
#include "base/callback.h"
#include "chrome/common/channel_info.h"
#include "chrome/services/util_win/public/mojom/constants.mojom.h"
#include "components/version_info/channel.h"
#include "services/service_manager/public/cpp/connector.h"

namespace {

// This is an undocumented structure returned from querying the "productState"
// uint32 from the AntiVirusProduct in WMI.
// http://neophob.com/2010/03/wmi-query-windows-securitycenter2/ gives a good
// summary and testing was also done with a variety of AV products to determine
// these values as accurately as possible.
#pragma pack(push)
#pragma pack(1)
struct PRODUCT_STATE {
  uint8_t unknown_1 : 4;
  uint8_t definition_state : 4;  // 1 = Out of date, 0 = Up to date.
  uint8_t unknown_2 : 4;
  uint8_t security_state : 4;  //  0 = Inactive, 1 = Active, 2 = Snoozed.
  uint8_t security_provider;   // matches WSC_SECURITY_PROVIDER in wscapi.h.
  uint8_t unknown_3;
};
#pragma pack(pop)

static_assert(sizeof(PRODUCT_STATE) == 4, "Wrong packing!");

bool ShouldReportFullNames() {
  // The expectation is that this will be disabled for the majority of users,
  // but this allows a small group to be enabled on other channels if there are
  // a large percentage of hashes collected on these channels that are not
  // resolved to names previously collected on Canary channel.
  bool enabled = base::FeatureList::IsEnabled(
      AntiVirusMetricsProvider::kReportNamesFeature);

  if (chrome::GetChannel() == version_info::Channel::CANARY)
    return true;

  return enabled;
}

}  // namespace

constexpr base::Feature AntiVirusMetricsProvider::kReportNamesFeature;

AntiVirusMetricsProvider::AntiVirusMetricsProvider(
    service_manager::Connector* connector)
    : connector_(connector) {}

AntiVirusMetricsProvider::~AntiVirusMetricsProvider() = default;

void AntiVirusMetricsProvider::ProvideSystemProfileMetrics(
    metrics::SystemProfileProto* system_profile_proto) {
  for (const auto& av_product : av_products_) {
    metrics::SystemProfileProto_AntiVirusProduct* product =
        system_profile_proto->add_antivirus_product();
    *product = av_product;
  }
}

void AntiVirusMetricsProvider::AsyncInit(const base::Closure& done_callback) {
  connector_->BindInterface(chrome::mojom::kUtilWinServiceName, &util_win_ptr_);

  // Intentionally don't handle connection errors as not reporting this metric
  // is acceptable in the rare cases it'll happen.
  // The usage of base::Unretained(this) is safe here because |util_win_ptr_|,
  // who owns the callback, will be destoyed once this instance goes away.
  util_win_ptr_->GetAntiVirusProducts(
      ShouldReportFullNames(),
      base::BindOnce(&AntiVirusMetricsProvider::GotAntiVirusProducts,
                     base::Unretained(this), done_callback));
}

void AntiVirusMetricsProvider::GotAntiVirusProducts(
    const base::Closure& done_callback,
    const std::vector<metrics::SystemProfileProto::AntiVirusProduct>& result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  util_win_ptr_ = nullptr;
  av_products_ = result;
  done_callback.Run();
}
