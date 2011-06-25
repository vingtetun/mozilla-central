/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla SpiderMonkey JavaScript 1.9 code, released
 * May 28, 2008.
 *
 * The Initial Developer of the Original Code is
 *   Mozilla Foundation
 * Portions created by the Initial Developer are Copyright (C) 2009
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Andreas Gal <gal@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#ifndef jsweakmap_h___
#define jsweakmap_h___

#include "jsapi.h"
#include "jscntxt.h"
#include "jsobj.h"

namespace js {

// A subclass template of js::HashMap whose keys and values may be garbage-collected. When
// a key is collected, the table entry disappears, dropping its reference to the value.
//
// More precisely:
//
//     A WeakMap entry is collected if and only if either the WeakMap or the entry's key
//     is collected. If an entry is not collected, it remains in the WeakMap and it has a
//     strong reference to the value.
//
// You must call this table's 'mark' method when the object of which it is a part is
// reached by the garbage collection tracer. Once a table is known to be live, the
// implementation takes care of the iterative marking needed for weak tables and removing
// table entries when collection is complete.
//
// You may provide your own MarkPolicy class to specify how keys and values are marked; a
// policy template provides default definitions for some common key/value type
// combinations.
//
// Details:
//
// The interface is as for a js::HashMap, with the following additions:
//
// - You must call the WeakMap's 'trace' member function when you discover that the map is
//   part of a live object. (You'll typically call this from the containing type's 'trace'
//   function.)
//
// - There is no AllocPolicy parameter; these are used with our garbage collector, so
//   RuntimeAllocPolicy is hard-wired.
//
// - Optional fourth template parameter is a class MarkPolicy, with the following constructor:
//   
//     MarkPolicy(JSTracer *)
//
//   and the following static member functions:
//
//     bool keyMarked(Key &k)
//     bool valueMarked(Value &v)
//        Return true if k/v has been marked as reachable by the collector, false otherwise.
//     void markKey(Key &k, const char *description)
//     void markValue(Value &v, const char *description)
//        Mark k/v as reachable by the collector, using trc. Use description to identify
//        k/v in debugging. (markKey is used only for non-marking tracers, other code
//        using the GC heap tracing functions to map the heap for some purpose or other.)
//
//   If omitted, this parameter defaults to js::DefaultMarkPolicy<Key, Value>, a policy
//   template with the obvious definitions for some typical SpiderMonkey type combinations.

// A policy template holding default marking algorithms for common type combinations. This
// provides default types for WeakMap's MarkPolicy template parameter.
template <class Key, class Value> class DefaultMarkPolicy;

// Common base class for all WeakMap specializations. The collector uses this to call
// their markIteratively and sweep methods.
class WeakMapBase {
  public:
    WeakMapBase() : next(NULL) { }

    void trace(JSTracer *tracer) {
        if (IS_GC_MARKING_TRACER(tracer)) {
            // We don't do anything with a WeakMap at trace time. Rather, we wait until as
            // many keys as possible have been marked, and add ourselves to the list of
            // known-live WeakMaps to be scanned in the iterative marking phase, by
            // markAllIteratively.
            JSRuntime *rt = tracer->context->runtime;
            next = rt->gcWeakMapList;
            rt->gcWeakMapList = this;
        } else {
            // If we're not actually doing garbage collection, the keys won't be marked
            // nicely as needed by the true ephemeral marking algorithm --- custom tracers
            // must use their own means for cycle detection. So here we do a conservative
            // approximation: pretend all keys are live.
            nonMarkingTrace(tracer);
        }
    }

    // Garbage collector entry points.

    // Check all weak maps that have been marked as live so far in this garbage
    // collection, and mark the values of all entries that have become strong references
    // to them. Return true if we marked any new values, indicating that we need to make
    // another pass. In other words, mark my marked maps' marked members' mid-collection.
    static bool markAllIteratively(JSTracer *tracer);

    // Remove entries whose keys are dead from all weak maps marked as live in this
    // garbage collection.
    static void sweepAll(JSTracer *tracer);

  protected:
    // Instance member functions called by the above. Instantiations of WeakMap override
    // these with definitions appropriate for their Key and Value types.
    virtual void nonMarkingTrace(JSTracer *tracer) = 0;
    virtual bool markIteratively(JSTracer *tracer) = 0;
    virtual void sweep(JSTracer *tracer) = 0;

  private:
    // Link in a list of all the WeakMaps we have marked in this garbage collection,
    // headed by JSRuntime::gcWeakMapList.
    WeakMapBase *next;
};

template <class Key, class Value,
          class HashPolicy = DefaultHasher<Key>,
          class MarkPolicy = DefaultMarkPolicy<Key, Value> >
class WeakMap : public HashMap<Key, Value, HashPolicy, RuntimeAllocPolicy>, public WeakMapBase {
  private:
    typedef HashMap<Key, Value, HashPolicy, RuntimeAllocPolicy> Base;
    typedef typename Base::Range Range;
    typedef typename Base::Enum Enum;

  public:
    WeakMap(JSContext *cx) : Base(cx) { }

  private:
    void nonMarkingTrace(JSTracer *tracer) {
        MarkPolicy t(tracer);
        for (Range r = Base::all(); !r.empty(); r.popFront()) {
            t.markKey(r.front().key, "WeakMap entry key");
            t.markValue(r.front().value, "WeakMap entry value");
        }
    }

    bool markIteratively(JSTracer *tracer) {
        MarkPolicy t(tracer);
        bool markedAny = false;
        for (Range r = Base::all(); !r.empty(); r.popFront()) {
            /* If the key is alive, mark the value if needed. */
            if (!t.valueMarked(r.front().value) && t.keyMarked(r.front().key)) {
                t.markValue(r.front().value, "WeakMap entry with live key");
                /* We revived a value with children, we have to iterate again. */
                markedAny = true;
            }
        }
        return markedAny;
    }

    void sweep(JSTracer *tracer) {
        MarkPolicy t(tracer);
        for (Enum e(*this); !e.empty(); e.popFront()) {
            if (!t.keyMarked(e.front().key))
                e.removeFront();
        }
#if DEBUG
        // Once we've swept, all edges should stay within the known-live part of the graph.
        for (Range r = Base::all(); !r.empty(); r.popFront()) {
            JS_ASSERT(t.keyMarked(r.front().key));
            JS_ASSERT(t.valueMarked(r.front().value));
        }
#endif
    }
};

// Marking policy for maps from JSObject pointers to js::Values.
template <>
class DefaultMarkPolicy<JSObject *, Value> {
  private:
    JSTracer *tracer;
  public:
    DefaultMarkPolicy(JSTracer *t) : tracer(t) { }
    bool keyMarked(JSObject *k) { return !IsAboutToBeFinalized(tracer->context, k); }
    bool valueMarked(const Value &v) {
        if (v.isMarkable())
            return !IsAboutToBeFinalized(tracer->context, v.toGCThing());
        return true;
    }
    void markKey(JSObject *k, const char *description) {
        js::gc::MarkObject(tracer, *k, description);
    }
    void markValue(const Value &v, const char *description) {
        js::gc::MarkValue(tracer, v, description);
    }
};

// The class of JavaScript WeakMap objects.
extern Class WeakMapClass;

}

extern JSObject *
js_InitWeakMapClass(JSContext *cx, JSObject *obj);

#endif