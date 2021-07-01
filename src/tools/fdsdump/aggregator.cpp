#define XXH_INLINE_ALL

#include "aggregator.hpp"
#include "information_elements.hpp"
#include "xxhash.h"
#include <cstring>

static IPAddress
make_ipv4_address(uint8_t *address)
{
    IPAddress a = {};
    a.length = 4;
    std::memcpy(a.address, address, 4);
    return a;
}

static IPAddress
make_ipv6_address(uint8_t *address)
{
    IPAddress a = {};
    a.length = 16;
    std::memcpy(a.address, address, 16);
    return a;
}

static uint64_t
get_uint(fds_drec_field &field)
{
    uint64_t tmp;
    int rc = fds_get_uint_be(field.data, field.size, &tmp);
    assert(rc == FDS_OK);
    return tmp;
}

static int64_t
get_int(fds_drec_field &field)
{
    int64_t tmp;
    int rc = fds_get_int_be(field.data, field.size, &tmp);
    assert(rc == FDS_OK);
    return tmp;
}

static int
build_key(const ViewDefinition &view_def, fds_drec &drec, uint8_t *key_buffer)
{
    ViewValue *key_value = reinterpret_cast<ViewValue *>(key_buffer);
    fds_drec_field drec_field;

    for (const auto &view_field : view_def.key_fields) {
        
        switch (view_field.kind) {
        case ViewFieldKind::VerbatimKey:
            if (fds_drec_find(&drec, view_field.pen, view_field.id, &drec_field) == FDS_EOC) {
                return 0;
            }

            switch (view_field.data_type) {
            case DataType::Unsigned8:
                key_value->u8 = get_uint(drec_field);
                advance_value_ptr(key_value, sizeof(key_value->u8));
                break;
            case DataType::Unsigned16:
                key_value->u16 = get_uint(drec_field);
                advance_value_ptr(key_value, sizeof(key_value->u16));
                break;
            case DataType::Unsigned32:
                key_value->u32 = get_uint(drec_field);
                advance_value_ptr(key_value, sizeof(key_value->u32));
                break;
            case DataType::Unsigned64:
                key_value->u64 = get_uint(drec_field);
                advance_value_ptr(key_value, sizeof(key_value->u64));
                break;
            case DataType::Signed8:
                key_value->i8 = get_int(drec_field);
                advance_value_ptr(key_value, sizeof(key_value->i8));
                break;
            case DataType::Signed16:
                key_value->i16 = get_int(drec_field);
                advance_value_ptr(key_value, sizeof(key_value->i16));
                break;
            case DataType::Signed32:
                key_value->i32 = get_int(drec_field);
                advance_value_ptr(key_value, sizeof(key_value->i32));
                break;
            case DataType::Signed64:
                key_value->i64 = get_int(drec_field);
                advance_value_ptr(key_value, sizeof(key_value->i64));
                break;
            default: assert(0);
            }
            break;

        case ViewFieldKind::SourceIPAddressKey:
            if (fds_drec_find(&drec, IPFIX::iana, IPFIX::sourceIPv4Address, &drec_field) != FDS_EOC) {
                key_value->ip = make_ipv4_address(drec_field.data);
            } else if (fds_drec_find(&drec, IPFIX::iana, IPFIX::sourceIPv6Address, &drec_field) != FDS_EOC) {
                key_value->ip = make_ipv6_address(drec_field.data);
            } else {
                return 0;
            }
            advance_value_ptr(key_value, sizeof(key_value->ip));
            break;
        
        case ViewFieldKind::DestinationIPAddressKey:
            if (fds_drec_find(&drec, IPFIX::iana, IPFIX::destinationIPv4Address, &drec_field) != FDS_EOC) {
                key_value->ip = make_ipv4_address(drec_field.data);
            } else if (fds_drec_find(&drec, IPFIX::iana, IPFIX::destinationIPv6Address, &drec_field) != FDS_EOC) {
                key_value->ip = make_ipv6_address(drec_field.data);
            } else {
                return 0;
            }
            advance_value_ptr(key_value, sizeof(key_value->ip));
            break;
        
        default: assert(0);
        }

    }

    return 1;
}

static void
aggregate_value(const ViewField &aggregate_field, fds_drec &drec, ViewValue *&value)
{
    fds_drec_field drec_field;

    switch (aggregate_field.kind) {

    case ViewFieldKind::SumAggregate:
        if (fds_drec_find(&drec, aggregate_field.pen, aggregate_field.id, &drec_field) == FDS_EOC) {
            return;
        }

        switch (aggregate_field.data_type) {
        case DataType::Unsigned64:
            value->u64 += get_uint(drec_field);
            advance_value_ptr(value, sizeof(value->u64));
            break;
        case DataType::Signed64:
            value->i64 += get_int(drec_field);
            advance_value_ptr(value, sizeof(value->i64));
            break;
        default: assert(0);
        }
        break;
    
    case ViewFieldKind::FlowCount:
        value->u64++;
        advance_value_ptr(value, sizeof(value->u64));
        break;

    default: assert(0);
    
    }
}

Aggregator::Aggregator(ViewDefinition view_def) : 
    m_view_def(view_def), 
    m_buckets{nullptr}
{
}

void
Aggregator::process_record(fds_drec &drec)
{
    constexpr std::size_t key_buffer_size = 1024;
    assert(m_view_def.keys_size <= key_buffer_size);

    uint8_t key_buffer[key_buffer_size];

    if (!build_key(m_view_def, drec, key_buffer)) {
        return;
    }

    uint64_t hash = XXH3_64bits(key_buffer, sizeof(m_view_def.keys_size));
    std::size_t bucket_index = hash % BUCKETS_COUNT;
    AggregateRecord **arec = &m_buckets[bucket_index];

    for (;;) {
        if (*arec == nullptr) {
            break;
        }

        if ((*arec)->hash == hash && std::memcmp((*arec)->data, key_buffer, m_view_def.keys_size) == 0) {
            break;
        }

        arec = &(*arec)->next;
    }

    if (*arec == nullptr) {
        void *tmp = calloc(1, sizeof(AggregateRecord) + m_view_def.keys_size + m_view_def.values_size);
        if (!tmp) {
            throw std::bad_alloc{};
        }
        *arec = static_cast<AggregateRecord *>(tmp);
        (*arec)->hash = hash;
        m_records.push_back(*arec);
        std::memcpy((*arec)->data, key_buffer, m_view_def.keys_size);
    }

    ViewValue *value = reinterpret_cast<ViewValue *>((*arec)->data + m_view_def.keys_size);
    for (const auto &aggregate_field : m_view_def.value_fields) {
        aggregate_value(aggregate_field, drec, value);
    }
}