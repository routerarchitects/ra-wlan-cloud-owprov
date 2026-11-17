//
// Created by stephane bourque on 2021-08-11.
//

#pragma once

#include "framework/SubSystemServer.h"
#include "framework/utils.h"
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace OpenWifi {
	class SerialNumberCache : public SubSystemServer {
	  public:
		static auto instance() {
			static auto instance_ = new SerialNumberCache;
			return instance_;
		}

		int Start() override;
		void Stop() override;
		void AddSerialNumber(const std::string &SerialNumber,
							 [[maybe_unused]] const std::string &DeviceType);
		void DeleteSerialNumber(const std::string &SerialNumber);
		void FindNumbers(const std::string &SerialNumber, uint HowMany, std::vector<std::string> &A);
		inline std::vector<std::string> GetCacheCopy() {
			std::lock_guard G(Mutex_);
			return SNs_;
		}
		inline bool NumberExists(const std::string &SerialNumber) {
			std::lock_guard G(Mutex_);
			auto S = Poco::toLower(SerialNumber);
			return std::binary_search(SNs_.begin(), SNs_.end(), S);
		}

		static inline std::string ReverseSerialNumber(const std::string &S) {
			std::string ReversedString;
			std::copy(rbegin(S), rend(S), std::back_inserter(ReversedString));
			return ReversedString;
		}

	  private:
		std::vector<std::string> SNs_;
		std::vector<std::string> Reverse_SNs_;

		void ReturnNumbers(const std::string &S, uint HowMany, const std::vector<std::string> &SNArr,
						   std::vector<std::string> &A, bool ReverseResult);

		SerialNumberCache() noexcept
			: SubSystemServer("SerialNumberCache", "SNCACHE-SVR", "serialcache") {
			SNs_.reserve(2000);
		}
	};

	inline auto SerialNumberCache() { return SerialNumberCache::instance(); }

} // namespace OpenWifi
