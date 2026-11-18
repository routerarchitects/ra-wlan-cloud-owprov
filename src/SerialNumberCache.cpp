//
// Created by stephane bourque on 2021-08-11.
//

#include "SerialNumberCache.h"
#include "framework/utils.h"

namespace OpenWifi {

	int SerialNumberCache::Start() { return 0; }

	void SerialNumberCache::Stop() {}

	void SerialNumberCache::AddSerialNumber(const std::string &S,
											[[maybe_unused]] const std::string &DeviceType) {
		std::lock_guard G(Mutex_);
		std::string LowerS = Poco::toLower(S);
		if (LowerS.empty()) return;

		auto insert_point = std::lower_bound(SNs_.begin(), SNs_.end(), LowerS);
		if (insert_point == SNs_.end() || *insert_point != LowerS) {
			SNs_.insert(insert_point, LowerS);

			auto R = ReverseSerialNumber(LowerS);
			auto rev_insert_point = std::lower_bound(Reverse_SNs_.begin(), Reverse_SNs_.end(), R);
			Reverse_SNs_.insert(rev_insert_point, R);
		}
	}

	void SerialNumberCache::DeleteSerialNumber(const std::string &S) {
		std::lock_guard G(Mutex_);
		std::string LowerS = Poco::toLower(S);
		if (LowerS.empty()) return;

		auto It = std::lower_bound(SNs_.begin(), SNs_.end(), LowerS);
		if (It != SNs_.end() && *It == LowerS) {
			SNs_.erase(It);

			auto R = ReverseSerialNumber(LowerS);
			auto RIt = std::lower_bound(Reverse_SNs_.begin(), Reverse_SNs_.end(), R);
			if (RIt != Reverse_SNs_.end() && *RIt == R) {
				Reverse_SNs_.erase(RIt);
			}
		}
	}

	void SerialNumberCache::ReturnNumbers(const std::string &S, uint HowMany,
										  const std::vector<std::string> &SNArr,
										  std::vector<std::string> &A, bool ReverseResult) {
		std::lock_guard G(Mutex_);

		auto LB = std::lower_bound(SNArr.begin(), SNArr.end(), S);
		for (; LB != SNArr.end() && HowMany > 0; ++LB) {
			if (LB->rfind(S, 0) == 0) {
				A.push_back(ReverseResult ? ReverseSerialNumber(*LB) : *LB);
				--HowMany;
			} else {
				break;
			}
		}
	}

	void SerialNumberCache::FindNumbers(const std::string &S, uint HowMany,
										std::vector<std::string> &A) {
		std::string LowerS = Poco::toLower(S);
		if (LowerS.empty())
			return;

		if (LowerS[0] == '*') {
			std::string Suffix = LowerS.substr(1);
			std::string Reversed = ReverseSerialNumber(Suffix);
			if (Reversed.empty())
				return;
			return ReturnNumbers(Reversed, HowMany, Reverse_SNs_, A, true);
		} else {
			return ReturnNumbers(LowerS, HowMany, SNs_, A, false);
		}
	}
} // namespace OpenWifi

