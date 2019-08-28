/***************************************************************************/
/*                                                                         */
/* Copyright 2019 INTERSEC SA                                              */
/*                                                                         */
/* Licensed under the Apache License, Version 2.0 (the "License");         */
/* you may not use this file except in compliance with the License.        */
/* You may obtain a copy of the License at                                 */
/*                                                                         */
/*     http://www.apache.org/licenses/LICENSE-2.0                          */
/*                                                                         */
/* Unless required by applicable law or agreed to in writing, software     */
/* distributed under the License is distributed on an "AS IS" BASIS,       */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*/
/* See the License for the specific language governing permissions and     */
/* limitations under the License.                                          */
/*                                                                         */
/***************************************************************************/

#include "zchk-iop.h"

#include "iop.h"
#include "iop/tstiop.iop.h"

#include "z.h"

/* Check the IOP_CORE_OBJ_DECLARE generates the right prototypes. */
static iop_core_obj_map_t *foo_mappings_g;
OBJ_CLASS(foo, iop_core_obj, IOP_CORE_OBJ_FIELDS, IOP_CORE_OBJ_METHODS,
          tstiop__mammal);
OBJ_VTABLE(foo)
OBJ_VTABLE_END()
IOP_CORE_OBJ_DECLARE(foo, tstiop__mammal);
IOP_CORE_OBJ_IMPL(foo_mappings_g, foo, tstiop__mammal)

#define MAMMAL_FIELDS(pfx, desc_type)                                        \
    IOP_CORE_OBJ_FIELDS(pfx, desc_type)
#define MAMMAL_METHODS(type_t, ...)                                          \
    IOP_CORE_OBJ_METHODS(type_t, ##__VA_ARGS__)

OBJ_CLASS(mammal, iop_core_obj, MAMMAL_FIELDS, MAMMAL_METHODS,
          tstiop__mammal);

OBJ_VTABLE(mammal)
OBJ_VTABLE_END()

#define FOX_FIELDS(pfx, desc_type)                                           \
    MAMMAL_FIELDS(pfx, desc_type)
#define FOX_METHODS(type_t, ...)                                             \
    MAMMAL_METHODS(type_t, ##__VA_ARGS__)

OBJ_CLASS(fox, mammal, FOX_FIELDS, FOX_METHODS, tstiop__fox);

OBJ_VTABLE(fox)
OBJ_VTABLE_END()

#define HOUND_FIELDS(pfx, desc_type)                                         \
    MAMMAL_FIELDS(pfx, desc_type)
#define HOUND_METHODS(type_t, ...)                                           \
    MAMMAL_METHODS(type_t, ##__VA_ARGS__)

OBJ_CLASS(hound, mammal, HOUND_FIELDS, HOUND_METHODS, tstiop__hound);

OBJ_VTABLE(hound)
OBJ_VTABLE_END()

static iop_core_obj_map_t *mammal_mappings_g;

IOP_CORE_OBJ_IMPL_STATIC(mammal_mappings_g, mammal, tstiop__mammal)

int test_iop_core_obj(void)
{
    mammal_t *rox;
    mammal_t *rouky;

    mammal_mappings_g = iop_core_obj_map_new();
    iop_core_obj_register(mammal, tstiop__fox, fox);
    iop_core_obj_register(mammal, tstiop__hound, hound);

    Z_ASSERT(cls_inherits(obj_class(fox), obj_class(mammal)));
    Z_ASSERT(cls_inherits(obj_class(hound), obj_class(mammal)));

    {
        t_scope;
        tstiop__fox__t *fox_desc = t_iop_new(tstiop__fox);
        tstiop__hound__t *hound_desc = t_iop_new(tstiop__hound);

        fox_desc->name = LSTR("Rox");
        rox = iop_core_obj_new(mammal, &fox_desc->super);

        hound_desc->name = LSTR("Rouky");
        rouky = iop_core_obj_new(mammal, &hound_desc->super);

        Z_ASSERT(iop_core_obj_get_cls(mammal, &fox_desc->super) ==
                 cls_cast(mammal, obj_class(fox)));
        Z_ASSERT(iop_core_obj_get_cls(mammal, &hound_desc->super) ==
                 cls_cast(mammal, obj_class(hound)));
    }

    Z_ASSERT(obj_is_a_class(rox, obj_class(fox)));
    Z_ASSERT(obj_is_a_class(rouky, obj_class(hound)));
    Z_ASSERT_LSTREQUAL(rox->desc->name, LSTR("Rox"));
    Z_ASSERT_LSTREQUAL(rouky->desc->name, LSTR("Rouky"));
    Z_ASSERT(iop_obj_is_a(rox->desc, tstiop__fox));
    Z_ASSERT(iop_obj_is_a(rouky->desc, tstiop__hound));

    obj_delete(&rox);
    obj_delete(&rouky);

    iop_core_obj_map_delete(&mammal_mappings_g);
    Z_HELPER_END;
}
