#include "serialization.h"
#include "array.h"
#include "dictionary.h"
#include "utils.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

typedef struct {
    unsigned char* start;
    unsigned char* end;
    unsigned char* ptr;
    size_t num_ports;
    spc_port_t* ports;
} writer_t;

typedef struct {
    unsigned char* end;
    unsigned char* ptr;
    size_t next_port;
    size_t num_ports;
    spc_port_t* ports;
    int error;
    unsigned depth;
} reader_t;

void spc_ensure_space(writer_t* writer, size_t num_bytes)
{
    if (writer->ptr + num_bytes > writer->end) {
        size_t new_size = (writer->end - writer->start) + num_bytes;
        void* buf = realloc(writer->start, new_size);
        writer->ptr = buf + (writer->ptr - writer->start);
        writer->start = buf;
        writer->end = buf + new_size;
    }
}

size_t spc_write(writer_t* writer, void* buf, size_t len)
{
    spc_ensure_space(writer, len);

    memcpy(writer->ptr, buf, len);
    writer->ptr += len;

    return len;
}

size_t spc_write_padded(writer_t* writer, const void* buf, size_t len)
{
    size_t remainder = (4 - (len % 4)) % 4;

    spc_ensure_space(writer, len + remainder);

    memcpy(writer->ptr, buf, len);
    writer->ptr += len;
    memset(writer->ptr, 0, remainder);
    writer->ptr += remainder;

    return len + remainder;
}

size_t spc_write_str(writer_t* writer, const char* str)
{
    return spc_write_padded(writer, str, strlen(str) + 1);
}

size_t spc_write_uint32(writer_t* writer, uint32_t val)
{
    // TODO replace with
    //    spc_write(writer, &val, 4);
    spc_ensure_space(writer, 4);

    memcpy(writer->ptr, &val, 4);
    writer->ptr += 4;

    return 4;
}

size_t spc_write_uint64(writer_t* writer, uint64_t val)
{
    spc_ensure_space(writer, 8);

    memcpy(writer->ptr, &val, 8);
    writer->ptr += 8;

    return 8;
}

size_t spc_write_int64(writer_t* writer, uint64_t val)
{
    spc_ensure_space(writer, 8);

    memcpy(writer->ptr, &val, 8);
    writer->ptr += 8;

    return 8;
}

size_t spc_write_double(writer_t* writer, double val)
{
    spc_ensure_space(writer, 8);

    memcpy(writer->ptr, &val, 8);
    writer->ptr += 8;

    return 8;
}

void spc_write_port(writer_t* writer, spc_port_t port)
{
    writer->ports = realloc(writer->ports, (writer->num_ports + 1) * sizeof(spc_port_t));
    writer->ports[writer->num_ports] = port;
    writer->num_ports += 1;
}

size_t spc_write_array(writer_t* writer, spc_array_t* array);
size_t spc_write_dict(writer_t* writer, spc_dictionary_t* dict);

size_t spc_serialize_value(writer_t* writer, spc_value_t value)
{
    size_t bytes_written = 0;
    bytes_written += spc_write_uint32(writer, value.type);
    switch (value.type) {
        case SPC_TYPE_NULL:
            break;
        case SPC_TYPE_BOOL:
            bytes_written += spc_write_uint32(writer, value.value.u64);
            break;
        case SPC_TYPE_UINT64:
            bytes_written += spc_write_uint64(writer, value.value.u64);
            break;
        case SPC_TYPE_INT64:
            bytes_written += spc_write_int64(writer, value.value.i64);
            break;
        case SPC_TYPE_DOUBLE:
            bytes_written += spc_write_double(writer, value.value.dbl);
            break;
        case SPC_TYPE_STRING:
            bytes_written += spc_write_uint32(writer, strlen(value.value.str) + 1);
            bytes_written += spc_write_str(writer, value.value.str);
            break;
        case SPC_TYPE_ARRAY:
            bytes_written += spc_write_array(writer, value.value.array);
            break;
        case SPC_TYPE_DICT:
            bytes_written += spc_write_dict(writer, value.value.dict);
            break;
        case SPC_TYPE_FD:
        case SPC_TYPE_SEND_PORT:
        case SPC_TYPE_ENDPOINT:
        case SPC_TYPE_RECV_PORT:
            spc_write_port(writer, value.value.port);
            break;
        case SPC_TYPE_UUID:
            bytes_written += spc_write(writer, value.value.ptr, 0x10);
            break;
        case SPC_TYPE_DATA:
            bytes_written += spc_write_uint32(writer, value.value.data.size);
            bytes_written += spc_write_padded(writer, value.value.data.ptr, value.value.data.size);
            break;
        default:
            printf("Unsupported value type: 0x%x\n", value.type);
    }

    return bytes_written;
}

size_t spc_write_array(writer_t* writer, spc_array_t* array)
{
    size_t bytes_written = 0;
    ptrdiff_t bytesize_offset = writer->ptr - writer->start;
    spc_write_uint32(writer, 0);            // placeholder for byte size

    bytes_written += spc_write_uint32(writer, array->length);

    for (size_t i = 0; i < array->length; i++) {
        bytes_written += spc_serialize_value(writer, array->values[i]);
    }

    // Fill in correct byte size
    *(uint32_t*)(writer->start + bytesize_offset) = bytes_written;
    return bytes_written + 4;
}

size_t spc_write_dict(writer_t* writer, spc_dictionary_t* dict)
{
    size_t bytes_written = 0;
    ptrdiff_t bytesize_offset = writer->ptr - writer->start;
    spc_write_uint32(writer, 0);            // placeholder for byte size

    bytes_written += spc_write_uint32(writer, dict->num_items);

    for (spc_dictionary_item_t* item = dict->items; item != NULL; item = item->next) {
        bytes_written += spc_write_str(writer, item->key);
        bytes_written += spc_serialize_value(writer, item->value);
    }

    // Fill in correct byte size
    *(uint32_t*)(writer->start + bytesize_offset) = bytes_written;
    return bytes_written + 4;
}

spc_mach_message_t* spc_serialize(spc_message_t* msg)
{
    spc_mach_message_t* mach_msg;

    size_t actual_size, content_size, initial_size = msg->content->num_items * 32;      // heuristic

    writer_t writer;
    writer.start = malloc(initial_size);
    writer.end = writer.start + initial_size;
    writer.ptr = writer.start;
    writer.ports = NULL;
    writer.num_ports = 0;

    spc_write(&writer, "CPX@\x05\x00\x00\x00", 8);
    spc_write_uint32(&writer, SPC_TYPE_DICT);
    spc_write_dict(&writer, msg->content);

    content_size = writer.ptr - writer.start;
    char* ptr;

    if (writer.num_ports != 0) {
        // Must create a complex messge
        actual_size = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) + writer.num_ports * sizeof(mach_msg_port_descriptor_t) + content_size;
        mach_msg = malloc(actual_size);
        mach_msg->header.msgh_bits = MACH_MSGH_BITS_COMPLEX;

        ptr = (char*)mach_msg + sizeof(mach_msg_header_t);
        mach_msg_body_t* body = (mach_msg_body_t*)ptr;
        body->msgh_descriptor_count = writer.num_ports;
        ptr += sizeof(mach_msg_body_t);

        for (size_t i = 0; i < writer.num_ports; i++) {
            mach_msg_port_descriptor_t* descriptor = (mach_msg_port_descriptor_t*)ptr;
            descriptor->type = MACH_MSG_PORT_DESCRIPTOR;
            descriptor->name = writer.ports[i].name;
            descriptor->disposition = writer.ports[i].type;
            ptr += sizeof(mach_msg_port_descriptor_t);
        }
    } else {
        actual_size = sizeof(mach_msg_header_t) + content_size;
        mach_msg = malloc(actual_size);
        mach_msg->header.msgh_bits = 0;
        ptr = (char*)mach_msg->buf;
    }

    // Fill in the mach message
    mach_msg->header.msgh_remote_port = msg->remote_port.name;
    mach_msg->header.msgh_local_port  = msg->local_port.name;
    mach_msg->header.msgh_id          = msg->id;
    mach_msg->header.msgh_size        = actual_size;
    mach_msg->header.msgh_bits       |= MACH_MSGH_BITS(msg->remote_port.type, msg->local_port.type);
    memcpy(ptr, writer.start, content_size);

    free(writer.start);
    free(writer.ports);

    return mach_msg;
}

/* All received bytes stay bounded by their transport-supplied span. Decode
 * errors belong to the message, not to the lifetime of its receiving service. */
static const void* spc_read(reader_t* reader, size_t bytes)
{
    if (reader->error || bytes > (size_t)(reader->end - reader->ptr)) {
        if (!reader->error) reader->error = EINVAL;
        return NULL;
    }
    const void* result = reader->ptr;
    reader->ptr += bytes;
    return result;
}
static uint32_t spc_read_uint32(reader_t* reader)
{
    uint32_t value = 0;
    const void* bytes = spc_read(reader, sizeof(value));
    if (bytes) memcpy(&value, bytes, sizeof(value));
    return value;
}
static uint64_t spc_read_uint64(reader_t* reader)
{
    uint64_t value = 0;
    const void* bytes = spc_read(reader, sizeof(value));
    if (bytes) memcpy(&value, bytes, sizeof(value));
    return value;
}
static const void* spc_read_padded(reader_t* reader, size_t bytes)
{
    if (bytes > SIZE_MAX - 3) { reader->error = EINVAL; return NULL; }
    return spc_read(reader, (bytes + 3) & ~(size_t)3);
}
static char* spc_read_key(reader_t* reader)
{
    if (reader->error) return NULL;
    const unsigned char* end = memchr(reader->ptr, 0, reader->end - reader->ptr);
    if (!end) { reader->error = EINVAL; return NULL; }
    const char* string = spc_read_padded(reader, end - reader->ptr + 1);
    if (!string) return NULL;
    char* result = strdup(string);
    if (!result) reader->error = ENOMEM;
    return result;
}
static spc_value_t spc_deserialize_value(reader_t* reader);

static spc_value_t spc_deserialize_container(reader_t* reader, uint32_t type)
{
    spc_value_t result = spc_null_create();
    uint32_t size = spc_read_uint32(reader);
    if (reader->error) return result;
    if (size < 4 || size > (size_t)(reader->end - reader->ptr)) {
        reader->error = EINVAL; return result;
    }
    unsigned char* outer_end = reader->end;
    reader->end = reader->ptr + size;
    uint32_t count = spc_read_uint32(reader);
    size_t minimum = type == SPC_TYPE_ARRAY ? 4 : 8;
    if (count > (size_t)(reader->end - reader->ptr) / minimum) reader->error = EINVAL;
    if (!reader->error && type == SPC_TYPE_ARRAY) {
        spc_array_t* array = spc_array_create();
        if (!array) { reader->error = ENOMEM; goto done; }
        result.type = type; result.value.array = array;
        array->values = calloc(count ? count : 1, sizeof(*array->values));
        if (!array->values) { reader->error = ENOMEM; goto done; }
        array->capacity = count;
        for (uint32_t i = 0; i < count && !reader->error; i++) {
            spc_value_t value = spc_deserialize_value(reader);
            array->values[array->length++] = value;
        }
    } else if (!reader->error) {
        spc_dictionary_t* dict = spc_dictionary_create();
        if (!dict) { reader->error = ENOMEM; goto done; }
        result.type = type; result.value.dict = dict;
        for (uint32_t i = 0; i < count && !reader->error; i++) {
            char* key = spc_read_key(reader);
            if (!key) break;
            spc_value_t value = spc_deserialize_value(reader);
            spc_dictionary_item_t* item = reader->error ? NULL : malloc(sizeof(*item));
            if (!item) {
                free(key); spc_value_destroy(value);
                if (!reader->error) reader->error = ENOMEM;
                break;
            }
            *item = (spc_dictionary_item_t){.key=key,.value=value,.next=dict->items};
            dict->items = item; dict->num_items++;
        }
    }
    if (!reader->error && reader->ptr != reader->end) reader->error = EINVAL;
done:
    reader->end = outer_end;
    return result;
}
static spc_value_t spc_deserialize_value(reader_t* reader)
{
    spc_value_t value = spc_null_create();
    if (reader->error) return value;
    if (reader->depth >= 64) { reader->error = EOVERFLOW; return value; }
    reader->depth++;
    uint32_t type = spc_read_uint32(reader);
    if (reader->error) goto done;
    switch (type) {
        case SPC_TYPE_NULL: break;
        case SPC_TYPE_BOOL:
            value.type = type; value.value.u64 = spc_read_uint32(reader); break;
        case SPC_TYPE_UINT64:
        case SPC_TYPE_INT64:
        case SPC_TYPE_DOUBLE:
            value.type = type; value.value.u64 = spc_read_uint64(reader); break;
        case SPC_TYPE_STRING: {
            uint32_t size = spc_read_uint32(reader);
            const char* bytes = size ? spc_read_padded(reader, size) : NULL;
            if (!bytes || memchr(bytes, 0, size) != bytes + size - 1) {
                reader->error = EINVAL; break;
            }
            char* string = malloc(size);
            if (!string) { reader->error = ENOMEM; break; }
            memcpy(string, bytes, size); value.type = type; value.value.str = string;
            break;
        }
        case SPC_TYPE_ARRAY:
        case SPC_TYPE_DICT:
            value = spc_deserialize_container(reader, type); break;
        case SPC_TYPE_FD:
        case SPC_TYPE_SEND_PORT:
        case SPC_TYPE_ENDPOINT:
        case SPC_TYPE_RECV_PORT:
            if (reader->next_port == reader->num_ports) { reader->error = EINVAL; break; }
            value.type = type; value.value.port = reader->ports[reader->next_port];
            reader->ports[reader->next_port++].name = MACH_PORT_NULL;
            break;
        case SPC_TYPE_DATA:
        case SPC_TYPE_UUID: {
            uint32_t size = type == SPC_TYPE_UUID ? 16 : spc_read_uint32(reader);
            const void* bytes = spc_read_padded(reader, size);
            if (!bytes) break;
            void* copy = malloc(size ? size : 1);
            if (!copy) { reader->error = ENOMEM; break; }
            memcpy(copy, bytes, size); value.type = type;
            if (type == SPC_TYPE_DATA) value.value.data = (spc_data_t){copy, size};
            else value.value.ptr = copy;
            break;
        }
        default: reader->error = ENOTSUP; break;
    }
done:
    reader->depth--;
    return value;
}

spc_message_t* spc_deserialize_payload(const mach_msg_header_t* header,
    const void* payload, size_t length, spc_port_t* ports, size_t count, int* error)
{
    reader_t reader = {.ptr=(unsigned char*)payload,.end=(unsigned char*)payload+length,
        .ports=ports,.num_ports=count};
    spc_value_t value = spc_null_create();
    spc_message_t* result = NULL;
    const void* magic = spc_read(&reader, 8);
    if (!magic || memcmp(magic, "CPX@\x05\x00\x00\x00", 8)) reader.error = EINVAL;
    if (!reader.error) value = spc_deserialize_value(&reader);
    if (!reader.error && value.type != SPC_TYPE_DICT) reader.error = EINVAL;
    if (!reader.error && reader.next_port != count) reader.error = EINVAL;
    if (!reader.error) {
        result = calloc(1, sizeof(*result));
        if (!result) reader.error = ENOMEM;
    }
    if (reader.error) spc_value_destroy(value);
    else {
        result->remote_port = (spc_port_t){header->msgh_remote_port,MACH_MSGH_BITS_REMOTE(header->msgh_bits)};
        result->local_port = (spc_port_t){header->msgh_local_port,MACH_MSGH_BITS_LOCAL(header->msgh_bits)};
        result->id = header->msgh_id; result->content = value.value.dict;
    }
    /* Values own consumed rights. Release unconsumed rights on either outcome. */
    for (size_t i = 0; i < count; i++) if (ports[i].name) {
        mach_port_deallocate(mach_task_self(), ports[i].name); ports[i].name = MACH_PORT_NULL;
    }
    if (error) *error = reader.error;
    return result;
}
spc_message_t* spc_deserialize(spc_mach_message_t* message)
{
    size_t offset = sizeof(message->header), length = message->header.msgh_size;
    if (length < offset) return NULL;
    size_t count = 0;
    spc_port_t* ports = NULL;
    if (message->header.msgh_bits & MACH_MSGH_BITS_COMPLEX) {
        if (length - offset < sizeof(mach_msg_body_t)) return NULL;
        mach_msg_body_t body; memcpy(&body, (char*)message+offset, sizeof(body));
        offset += sizeof(body); count = body.msgh_descriptor_count;
        if (count > (length-offset) / sizeof(mach_msg_port_descriptor_t)) return NULL;
        ports = calloc(count ? count : 1, sizeof(*ports));
        if (!ports) return NULL;
        /* Non-inline transport is owned by the caller via deserialize_payload;
         * never ignore an OOL descriptor or guess where its data lives. */
        for (size_t i = 0; i < count; i++) {
            mach_msg_port_descriptor_t descriptor;
            memcpy(&descriptor, (char*)message+offset+i*sizeof(descriptor), sizeof(descriptor));
            if (descriptor.type != MACH_MSG_PORT_DESCRIPTOR) { free(ports); return NULL; }
            ports[i] = (spc_port_t){descriptor.name,descriptor.disposition};
        }
        offset += count * sizeof(mach_msg_port_descriptor_t);
    }
    spc_message_t* result = spc_deserialize_payload(&message->header, (char*)message+offset,
        length-offset, ports, count, NULL);
    free(ports);
    return result;
}
