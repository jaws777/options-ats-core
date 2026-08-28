#include "core/fixed_price.hpp"
#include "market_data/opra.hpp"

#include <array>
#include <iostream>
#include <string_view>

namespace {

using options_ats_core::core::FixedPrice;
using options_ats_core::market_data::OptionRight;
using options_ats_core::market_data::parse_exchange_quote_line;

constexpr std::array<std::string_view, 3> kSampleLines{
    "AAPL  240621C00195000,1718386800123456789,2.35,2.40,125,110,CBOE",
    "SPY   240621P00540000,1718386800123456890,4.10,4.18,50,45,C2",
    "QQQ   240621C00450000,1718386800123456999,6.25,6.30,80,75,EDGX",
};

std::string_view right_string(OptionRight right) noexcept {
    return right == OptionRight::Call ? "C" : "P";
}

} // namespace

int main() {
    for (const auto line : kSampleLines) {
        const auto quote = parse_exchange_quote_line(line);
        if (!quote.has_value()) {
            std::cerr << "parse error: " << static_cast<int>(quote.error()) << '\n';
            return 1;
        }

        std::array<char, FixedPrice::kCharsCapacity> strike_buf{};
        std::array<char, FixedPrice::kCharsCapacity> bid_buf{};
        std::array<char, FixedPrice::kCharsCapacity> ask_buf{};

        std::cout
            << "exchange=" << quote->exchange
            << " root=" << quote->symbol.root_view()
            << " expiry=20" << static_cast<int>(quote->symbol.expiry_year)
            << '-'
            << (quote->symbol.expiry_month < 10 ? "0" : "")
            << static_cast<int>(quote->symbol.expiry_month)
            << '-'
            << (quote->symbol.expiry_day < 10 ? "0" : "")
            << static_cast<int>(quote->symbol.expiry_day)
            << " right=" << right_string(quote->symbol.right)
            << " strike=" << quote->symbol.strike.to_string_view(strike_buf)
            << " bid=" << quote->bid.to_string_view(bid_buf)
            << " ask=" << quote->ask.to_string_view(ask_buf)
            << " bid_qty=" << quote->bid_qty
            << " ask_qty=" << quote->ask_qty
            << " ts_ns=" << quote->timestamp_ns
            << '\n';
    }

    return 0;
}
