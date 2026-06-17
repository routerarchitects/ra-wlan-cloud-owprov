#pragma once

#include <vector>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace OpenWifi::RESTAPI {

	template <typename Vec, typename Predicate>
	Vec FilterRecords(const Vec &items, Predicate predicate) {
		Vec filtered;
		for (const auto &item : items) {
			if (predicate(item)) {
				filtered.push_back(item);
			}
		}
		return filtered;
	}

	template <typename Vec>
	Vec ApplyPagination(const Vec &items, uint64_t offset, uint64_t limit) {
		if (offset >= items.size()) {
			return {};
		}

		auto start = static_cast<std::size_t>(offset);
		auto end = limit == 0
			? start
			: std::min<std::size_t>(
				  items.size(),
				  start + static_cast<std::size_t>(limit));

		return Vec(items.begin() + start, items.begin() + end);
	}

} // namespace OpenWifi::RESTAPI
