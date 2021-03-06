/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/macro.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/hook-list.h>
#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/modargs.h>

#include "stream-interaction.h"

struct userdata {
    pa_core *core;
    pa_hashmap *cork_state;
    pa_idxset *trigger_roles;
    pa_idxset *cork_roles;
    bool global:1;
    pa_hook_slot
        *sink_input_put_slot,
        *sink_input_unlink_slot,
        *sink_input_move_start_slot,
        *sink_input_move_finish_slot,
        *sink_input_state_changed_slot,
        *sink_input_mute_changed_slot,
        *sink_input_proplist_changed_slot;
};

static const char *is_trigger_stream(struct userdata *u, pa_sink_input *i) {
    const char *role, *trigger_role;
    uint32_t role_idx;

    if (!(role = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_ROLE)))
        role = "no_role";

    PA_IDXSET_FOREACH(trigger_role, u->trigger_roles, role_idx) {
        if (pa_streq(role, trigger_role))
            return trigger_role;
    }
    return NULL;
}

static bool shall_cork(struct userdata *u, pa_sink *s, pa_sink_input *ignore) {
    pa_sink_input *j;
    uint32_t idx;
    const char *trigger_role;

    pa_assert(u);
    pa_sink_assert_ref(s);

    for (j = PA_SINK_INPUT(pa_idxset_first(s->inputs, &idx)); j; j = PA_SINK_INPUT(pa_idxset_next(s->inputs, &idx))) {

        if (j == ignore)
            continue;

        trigger_role = is_trigger_stream(u, j);
        if (trigger_role && !j->muted && pa_sink_input_get_state(j) != PA_SINK_INPUT_CORKED) {
            pa_log_debug("Found a '%s' stream that will trigger the auto-cork.", trigger_role);
            return true;
        }
    }

    return false;
}

static void cork_stream(struct userdata *u, pa_sink_input *i, const char *cork_role) {

    pa_log_debug("Found a '%s' stream that should be corked/muted.", cork_role);
    pa_sink_input_set_mute(i, true, false);
    pa_sink_input_send_event(i, PA_STREAM_EVENT_REQUEST_CORK, NULL);
}

static void uncork_stream(struct userdata *u, pa_sink_input *i, const char *cork_role, bool corked) {

    pa_log_debug("Found a '%s' stream that should be uncorked/unmuted.", cork_role);
    if (corked || i->muted) {
       if (i->muted)
          pa_sink_input_set_mute(i, false, false);
       if (corked)
          pa_sink_input_send_event(i, PA_STREAM_EVENT_REQUEST_UNCORK, NULL);
    }
}

static inline void apply_cork_to_sink(struct userdata *u, pa_sink *s, pa_sink_input *ignore, bool cork) {
    pa_sink_input *j;
    uint32_t idx, role_idx;
    const char *cork_role;
    bool trigger = false;

    pa_assert(u);
    pa_sink_assert_ref(s);

    for (j = PA_SINK_INPUT(pa_idxset_first(s->inputs, &idx)); j; j = PA_SINK_INPUT(pa_idxset_next(s->inputs, &idx))) {
        bool corked, corked_here;
        const char *role;

        if (j == ignore)
            continue;

        if (!(role = pa_proplist_gets(j->proplist, PA_PROP_MEDIA_ROLE)))
            role = "no_role";

        PA_IDXSET_FOREACH(cork_role, u->cork_roles, role_idx) {
            if ((trigger = pa_streq(role, cork_role)))
                break;
            if ((trigger = (pa_streq(cork_role, "any_role") && !is_trigger_stream(u, j))))
                break;
        }
        if (!trigger)
            continue;

        corked = (pa_sink_input_get_state(j) == PA_SINK_INPUT_CORKED);
        corked_here = !!pa_hashmap_get(u->cork_state, j);

        if (cork && !corked && !j->muted) {
            if (!corked_here)
                pa_hashmap_put(u->cork_state, j, PA_INT_TO_PTR(1));

            cork_stream(u, j, role);
        } else if (!cork && corked_here) {
            pa_hashmap_remove(u->cork_state, j);

            uncork_stream(u, j, role, corked);
        }
    }
}

static void apply_cork(struct userdata *u, pa_sink *s, pa_sink_input *ignore, bool cork) {
    pa_assert(u);

    if (u->global) {
        uint32_t idx;
        PA_IDXSET_FOREACH(s, u->core->sinks, idx)
            apply_cork_to_sink(u, s, ignore, cork);
    } else
        apply_cork_to_sink(u, s, ignore, cork);
}

static void remove_cork(struct userdata *u) {
    uint32_t idx, idx_input;
    pa_sink *s;
    pa_sink_input *j;
    bool corked;
    const char *role;

    PA_IDXSET_FOREACH(s, u->core->sinks, idx) {

      for (j = PA_SINK_INPUT(pa_idxset_first(s->inputs, &idx_input)); j; j = PA_SINK_INPUT(pa_idxset_next(s->inputs, &idx_input))) {

         if(!!pa_hashmap_get(u->cork_state, j)) {
           corked = (pa_sink_input_get_state(j) == PA_SINK_INPUT_CORKED);
           if (!(role = pa_proplist_gets(j->proplist, PA_PROP_MEDIA_ROLE)))
              role = "no_role";
           uncork_stream(u, j, role, corked);
         }
      }
   }
}

static pa_hook_result_t process(struct userdata *u, pa_sink_input *i, bool create) {
    bool cork = false;

    pa_assert(u);
    pa_sink_input_assert_ref(i);

    if (!create)
        pa_hashmap_remove(u->cork_state, i);

    if (!i->sink)
        return PA_HOOK_OK;

    cork = shall_cork(u, i->sink, create ? NULL : i);
    apply_cork(u, i->sink, create ? NULL : i, cork);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_put_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i, true);
}

static pa_hook_result_t sink_input_unlink_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_sink_input_assert_ref(i);

    return process(u, i, false);
}

static pa_hook_result_t sink_input_move_start_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i, false);
}

static pa_hook_result_t sink_input_move_finish_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i, true);
}

static pa_hook_result_t sink_input_state_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(i)) && is_trigger_stream(u, i))
        return process(u, i, true);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_mute_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(i)) && is_trigger_stream(u, i))
        return process(u, i, true);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_proplist_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(pa_sink_input_get_state(i)))
        return process(u, i, true);

    return PA_HOOK_OK;
}

int pa_cork_init(pa_module *m, const char* const v_modargs[]) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    const char *roles;
    bool global = false;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, v_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);

    u->core = m->core;
    u->cork_state = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);

    u->trigger_roles = pa_idxset_new(NULL, NULL);
    roles = pa_modargs_get_value(ma, "trigger_roles", NULL);
    if (roles) {
        const char *split_state = NULL;
        char *n = NULL;
        while ((n = pa_split(roles, ",", &split_state))) {
            if (n[0] != '\0')
                pa_idxset_put(u->trigger_roles, n, NULL);
            else
                pa_xfree(n);
        }
    }
    if (pa_idxset_isempty(u->trigger_roles)) {
        pa_log_debug("Using role 'phone' as trigger role.");
        pa_idxset_put(u->trigger_roles, pa_xstrdup("phone"), NULL);
    }

    u->cork_roles = pa_idxset_new(NULL, NULL);
    roles = pa_modargs_get_value(ma, "cork_roles", NULL);
    if (roles) {
        const char *split_state = NULL;
        char *n = NULL;
        while ((n = pa_split(roles, ",", &split_state))) {
            if (n[0] != '\0')
                pa_idxset_put(u->cork_roles, n, NULL);
            else
                pa_xfree(n);
        }
    }
    if (pa_idxset_isempty(u->cork_roles)) {
        pa_log_debug("Using roles 'music' and 'video' as cork roles.");
        pa_idxset_put(u->cork_roles, pa_xstrdup("music"), NULL);
        pa_idxset_put(u->cork_roles, pa_xstrdup("video"), NULL);
    }

    if (pa_modargs_get_value_boolean(ma, "global", &global) < 0) {
        pa_log("Invalid boolean parameter: global");
        goto fail;
    }
    u->global = global;

    u->sink_input_put_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_put_cb, u);
    u->sink_input_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_unlink_cb, u);
    u->sink_input_move_start_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_START], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_start_cb, u);
    u->sink_input_move_finish_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_finish_cb, u);
    u->sink_input_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_state_changed_cb, u);
    u->sink_input_mute_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MUTE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_mute_changed_cb, u);
    u->sink_input_proplist_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PROPLIST_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_proplist_changed_cb, u);

    pa_modargs_free(ma);

    return 0;

fail:
    pa_cork_done(m);

    if (ma)
        pa_modargs_free(ma);

    return -1;

}

void pa_cork_done(pa_module *m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->trigger_roles)
        pa_idxset_free(u->trigger_roles, pa_xfree);

    if (u->cork_roles)
        pa_idxset_free(u->cork_roles, pa_xfree);

    if (u->sink_input_put_slot)
        pa_hook_slot_free(u->sink_input_put_slot);
    if (u->sink_input_unlink_slot)
        pa_hook_slot_free(u->sink_input_unlink_slot);
    if (u->sink_input_move_start_slot)
        pa_hook_slot_free(u->sink_input_move_start_slot);
    if (u->sink_input_move_finish_slot)
        pa_hook_slot_free(u->sink_input_move_finish_slot);
    if (u->sink_input_state_changed_slot)
        pa_hook_slot_free(u->sink_input_state_changed_slot);
    if (u->sink_input_mute_changed_slot)
        pa_hook_slot_free(u->sink_input_mute_changed_slot);
    if (u->sink_input_proplist_changed_slot)
        pa_hook_slot_free(u->sink_input_proplist_changed_slot);

    if (u->cork_state) {
        remove_cork(u);
        pa_hashmap_free(u->cork_state);
    }

    pa_xfree(u);

}
