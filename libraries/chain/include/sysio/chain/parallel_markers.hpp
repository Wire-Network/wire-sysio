#pragma once

#include <algorithm>
#include <ranges>

namespace sysio { namespace chain {

/**
 * @brief Return values in DataRange corresponding to matching Markers
 *
 * Takes two parallel ranges, a Data range containing data values, and a Marker range containing markers on the
 * corresponding data values. Returns a new Data range containing only the values corresponding to markers which match
 * markerValue
 *
 * For example:
 * @code{.cpp}
 * vector<char> data = {'A', 'B', 'C'};
 * vector<bool> markers = {true, false, true};
 * auto markedData = FilterDataByMarker(data, markers, true);
 * // markedData contains {'A', 'C'}
 * @endcode
 */
template<typename DataRange, typename MarkerRange, typename Marker>
DataRange filter_data_by_marker(DataRange data, MarkerRange markers, const Marker& markerValue) {
   auto zipped = std::views::zip(markers, data)
               | std::views::filter([&markerValue](const auto& tuple) {
                    return std::get<0>(tuple) == markerValue;
                 })
               | std::views::transform([](const auto& tuple) {
                    return std::get<1>(tuple);
                 });
   return {zipped.begin(), zipped.end()};
}

}} // namespace sysio::chain
