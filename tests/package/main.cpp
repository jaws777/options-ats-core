#include <core/fixed_price.hpp>

int main() {
    constexpr auto price = options_ats_core::core::FixedPrice::from_raw(123'450);
    return price.to_raw() == 123'450 ? 0 : 1;
}
