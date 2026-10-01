#pragma once

#include <assert.h>
#include <glib-object.h>
#include <js.h>
#include <stdint.h>

static const js_type_tag_t bare_gobject__carrier = {0x8f1d3a6c5e204b97, 0xb4e70c9218d3a65f};

typedef struct {
  GObject *object;
  js_ref_t *wrapper;
  uint32_t claims;
} bare_gobject_entry_t;

typedef struct {
  uint32_t tag;
  js_ref_t *wrapper;
} bare_gobject__claim_t;

typedef struct {
  GHashTable *entries;
  GHashTable *tags;

  uint32_t next_tag;
  uint32_t refs;
} bare_gobject_registry_t;

/**
 * Keep `registry` alive, for example while native code holds on to it. Give the
 * reference back with `bare_gobject_registry_release()`.
 */
static void
bare_gobject_registry_retain(bare_gobject_registry_t *registry) {
  registry->refs++;
}

/**
 * Give back a reference to `registry`. The registry is freed when the last
 * reference is gone.
 */
static void
bare_gobject_registry_release(bare_gobject_registry_t *registry) {
  assert(registry->refs > 0);

  if (--registry->refs > 0) return;

  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init(&iter, registry->entries);

  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    g_object_unref(((bare_gobject_entry_t *) value)->object);
  }

  g_hash_table_destroy(registry->entries);
  g_hash_table_destroy(registry->tags);

  g_free(registry);
}

static void
bare_gobject__on_registry_release(js_env_t *env, void *data, void *finalize_hint) {
  bare_gobject_registry_release(data);
}

/**
 * Create a registry for an addon, and pass it as the data pointer of every
 * function the addon exports. The registry lives as long as `exports` and every
 * token it has handed out.
 */
static bare_gobject_registry_t *
bare_gobject_registry_create(js_env_t *env, js_value_t *exports) {
  int err;

  bare_gobject_registry_t *registry = g_new0(bare_gobject_registry_t, 1);

  registry->entries = g_hash_table_new_full(NULL, NULL, NULL, g_free);
  registry->tags = g_hash_table_new(NULL, NULL);

  registry->next_tag = 1;
  registry->refs = 1;

  err = js_add_finalizer(env, exports, registry, bare_gobject__on_registry_release, NULL, NULL);
  assert(err == 0);

  return registry;
}

/**
 * Give `object` a tag and return it. An instance that already has a tag keeps
 * it. The registry takes its own reference, and sinks an instance that starts
 * out floating, so the registry always ends up with exactly one reference.
 */
static uint32_t
bare_gobject_tag(bare_gobject_registry_t *registry, gpointer object) {
  gpointer existing;

  if (g_hash_table_lookup_extended(registry->tags, object, NULL, &existing)) {
    return GPOINTER_TO_UINT(existing);
  }

  uint32_t tag = registry->next_tag++;

  bare_gobject_entry_t *entry = g_new0(bare_gobject_entry_t, 1);

  entry->object = g_object_ref_sink(object);

  g_hash_table_insert(registry->entries, GUINT_TO_POINTER(tag), entry);
  g_hash_table_insert(registry->tags, object, GUINT_TO_POINTER(tag));

  return tag;
}

static bare_gobject_entry_t *
bare_gobject__entry(bare_gobject_registry_t *registry, uint32_t tag) {
  return g_hash_table_lookup(registry->entries, GUINT_TO_POINTER(tag));
}

static js_value_t *
bare_gobject__wrapper(js_env_t *env, bare_gobject_entry_t *entry) {
  if (entry->wrapper == NULL) return NULL;

  js_value_t *result;
  int err = js_get_reference_value(env, entry->wrapper, &result);
  assert(err == 0);

  return result;
}

/**
 * Return the instance for `tag`, or `NULL` if there is none.
 */
static gpointer
bare_gobject_object(bare_gobject_registry_t *registry, uint32_t tag) {
  bare_gobject_entry_t *entry = bare_gobject__entry(registry, tag);

  if (entry == NULL) return NULL;

  return entry->object;
}

static int
bare_gobject__read_uint32(js_env_t *env, js_value_t *value, const char *name, uint32_t *result) {
  int err;

  bool is;
  err = js_is_number(env, value, &is);
  assert(err == 0);

  if (!is) {
    err = js_throw_type_errorf(env, NULL, "Expected '%s' to be a number", name);
    assert(err == 0);

    return -1;
  }

  err = js_get_value_uint32(env, value, result);
  assert(err == 0);

  return 0;
}

/**
 * Read the instance for the tag in `value` into `result` and return 0. If
 * `value` is not a number or not a known tag, throw a JavaScript error that
 * mentions `name` and return -1.
 */
static int
bare_gobject_read_tag(js_env_t *env, bare_gobject_registry_t *registry, js_value_t *value, const char *name, gpointer *result) {
  int err;

  uint32_t tag;
  err = bare_gobject__read_uint32(env, value, name, &tag);
  if (err < 0) return err;

  bare_gobject_entry_t *entry = bare_gobject__entry(registry, tag);

  if (entry == NULL) {
    err = js_throw_errorf(env, NULL, "Unknown tag %u", tag);
    assert(err == 0);

    return -1;
  }

  *result = entry->object;

  return 0;
}

/**
 * Like `bare_gobject_read_tag()`, but also throw a `TypeError` and return -1 if
 * the instance is not a `type`.
 */
static int
bare_gobject_read_type(js_env_t *env, bare_gobject_registry_t *registry, js_value_t *value, const char *name, GType type, gpointer *result) {
  int err;

  gpointer object;
  err = bare_gobject_read_tag(env, registry, value, name, &object);
  if (err < 0) return err;

  if (!G_TYPE_CHECK_INSTANCE_TYPE(object, type)) {
    err = js_throw_type_errorf(env, NULL, "Expected '%s' to be a %s, not a %s", name, g_type_name(type), G_OBJECT_TYPE_NAME(object));
    assert(err == 0);

    return -1;
  }

  *result = object;

  return 0;
}

/**
 * Return the JavaScript wrapper of `object`, or `NULL` if it has none.
 */
static js_value_t *
bare_gobject_lookup(js_env_t *env, bare_gobject_registry_t *registry, gpointer object) {
  gpointer tag;

  if (!g_hash_table_lookup_extended(registry->tags, object, NULL, &tag)) return NULL;

  bare_gobject_entry_t *entry = g_hash_table_lookup(registry->entries, tag);

  if (entry == NULL) return NULL;

  return bare_gobject__wrapper(env, entry);
}

static void
bare_gobject__on_token_finalize(js_env_t *env, void *data, void *finalize_hint) {
  int err;

  bare_gobject_registry_t *registry = finalize_hint;

  bare_gobject__claim_t *claim = (bare_gobject__claim_t *) data;

  uint32_t tag = claim->tag;

  err = js_delete_reference(env, claim->wrapper);
  assert(err == 0);

  bare_gobject_entry_t *entry = bare_gobject__entry(registry, tag);

  if (entry->wrapper == claim->wrapper) entry->wrapper = NULL;

  g_free(claim);

  if (--entry->claims == 0) {
    GObject *object = entry->object;

    g_hash_table_remove(registry->tags, object);
    g_hash_table_remove(registry->entries, GUINT_TO_POINTER(tag));

    g_object_unref(object);
  }

  bare_gobject_registry_release(registry);
}

/**
 * Export as `claim(tag, wrapper)`. Return a token that keeps the instance alive
 * until the token is garbage collected. A tag can be claimed more than once,
 * and the instance stays alive until every token is gone. The registry does not
 * keep any wrapper alive.
 */
static js_value_t *
bare_gobject_claim(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  bare_gobject_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  assert(argc == 2);

  uint32_t tag;
  err = bare_gobject__read_uint32(env, argv[0], "tag", &tag);
  if (err < 0) return NULL;

  bare_gobject_entry_t *entry = bare_gobject__entry(registry, tag);

  if (entry == NULL) {
    err = js_throw_errorf(env, NULL, "Unknown tag %u", tag);
    assert(err == 0);

    return NULL;
  }

  bare_gobject__claim_t *claim = g_new(bare_gobject__claim_t, 1);

  claim->tag = tag;

  err = js_create_reference(env, argv[1], 0, &claim->wrapper);
  assert(err == 0);

  if (bare_gobject__wrapper(env, entry) == NULL) entry->wrapper = claim->wrapper;

  entry->claims++;

  bare_gobject_registry_retain(registry);

  js_value_t *token;
  err = js_create_external(env, claim, bare_gobject__on_token_finalize, registry, &token);
  assert(err == 0);

  return token;
}

/**
 * Export as `wrapper(tag)`. Return the first wrapper of `tag` that is still
 * alive, or `null` if there is none. An unknown tag is not an error, because
 * `adopt()` asks with a tag that may belong to another addon to find out
 * whether an object is ours.
 */
static js_value_t *
bare_gobject_wrapper(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  bare_gobject_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  assert(argc == 1);

  uint32_t tag;
  err = bare_gobject__read_uint32(env, argv[0], "tag", &tag);
  if (err < 0) return NULL;

  bare_gobject_entry_t *entry = bare_gobject__entry(registry, tag);

  js_value_t *result = entry == NULL ? NULL : bare_gobject__wrapper(env, entry);

  if (result == NULL) {
    err = js_get_null(env, &result);
    assert(err == 0);
  }

  return result;
}

/**
 * Export as `registrySize()`. Return the number of instances in the registry.
 * Useful for finding leaks in tests.
 */
static js_value_t *
bare_gobject_registry_size(js_env_t *env, js_callback_info_t *info) {
  int err;

  bare_gobject_registry_t *registry;
  err = js_get_callback_info(env, info, NULL, NULL, NULL, (void **) &registry);
  assert(err == 0);

  js_value_t *result;
  err = js_create_uint32(env, g_hash_table_size(registry->entries), &result);
  assert(err == 0);

  return result;
}

static void
bare_gobject__on_carrier_finalize(js_env_t *env, void *data, void *finalize_hint) {
  g_object_unref(data);
}

/**
 * Export as `handle(tag)`. Return a handle that another addon can adopt. The
 * handle holds its own reference to the instance.
 */
static js_value_t *
bare_gobject_handle(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  bare_gobject_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  assert(argc == 1);

  gpointer object;
  err = bare_gobject_read_tag(env, registry, argv[0], "tag", &object);
  if (err < 0) return NULL;

  js_value_t *carrier;
  err = js_create_object(env, &carrier);
  assert(err == 0);

  err = js_wrap(env, carrier, g_object_ref(object), bare_gobject__on_carrier_finalize, NULL, NULL);
  assert(err == 0);

  err = js_add_type_tag(env, carrier, &bare_gobject__carrier);
  assert(err == 0);

  return carrier;
}

/**
 * Export as `adopt(handle)`. Return a tag for the instance in `handle`. A
 * instance that already has a tag keeps it.
 */
static js_value_t *
bare_gobject_adopt(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  bare_gobject_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &registry);
  assert(err == 0);

  assert(argc == 1);

  bool is;
  err = js_is_object(env, argv[0], &is);
  assert(err == 0);

  if (is) {
    err = js_check_type_tag(env, argv[0], &bare_gobject__carrier, &is);
    assert(err == 0);
  }

  if (!is) {
    err = js_throw_type_error(env, NULL, "Expected 'carrier' to be a GObject handle");
    assert(err == 0);

    return NULL;
  }

  gpointer object;
  err = js_unwrap(env, argv[0], &object);
  assert(err == 0);

  js_value_t *result;
  err = js_create_uint32(env, bare_gobject_tag(registry, object), &result);
  assert(err == 0);

  return result;
}
