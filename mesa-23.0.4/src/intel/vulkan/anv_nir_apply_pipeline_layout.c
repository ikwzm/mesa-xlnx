/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "anv_nir.h"
#include "program/prog_parameter.h"
#include "nir/nir_builder.h"
#include "compiler/brw_nir.h"
#include "util/mesa-sha1.h"
#include "util/set.h"

/* Sampler tables don't actually have a maximum size but we pick one just so
 * that we don't end up emitting too much state on-the-fly.
 */
#define MAX_SAMPLER_TABLE_SIZE 128
#define BINDLESS_OFFSET        255

#define sizeof_field(type, field) sizeof(((type *)0)->field)

struct apply_pipeline_layout_state {
   const struct anv_physical_device *pdevice;

   const struct anv_pipeline_layout *layout;
   bool add_bounds_checks;
   nir_address_format desc_addr_format;
   nir_address_format ssbo_addr_format;
   nir_address_format ubo_addr_format;

   /* Place to flag lowered instructions so we don't lower them twice */
   struct set *lowered_instrs;

   bool uses_constants;
   bool has_dynamic_buffers;
   uint8_t constants_offset;
   struct {
      bool desc_buffer_used;
      uint8_t desc_offset;

      uint8_t *use_count;
      uint8_t *surface_offsets;
      uint8_t *sampler_offsets;
   } set[MAX_SETS];
};

static nir_address_format
addr_format_for_desc_type(VkDescriptorType desc_type,
                          struct apply_pipeline_layout_state *state)
{
   switch (desc_type) {
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      return state->ssbo_addr_format;

   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      return state->ubo_addr_format;

   case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
      return state->desc_addr_format;

   default:
      unreachable("Unsupported descriptor type");
   }
}

static void
add_binding(struct apply_pipeline_layout_state *state,
            uint32_t set, uint32_t binding)
{
   const struct anv_descriptor_set_binding_layout *bind_layout =
      &state->layout->set[set].layout->binding[binding];

   if (state->set[set].use_count[binding] < UINT8_MAX)
      state->set[set].use_count[binding]++;

   /* Only flag the descriptor buffer as used if there's actually data for
    * this binding.  This lets us be lazy and call this function constantly
    * without worrying about unnecessarily enabling the buffer.
    */
   if (bind_layout->descriptor_stride)
      state->set[set].desc_buffer_used = true;
}

static void
add_deref_src_binding(struct apply_pipeline_layout_state *state, nir_src src)
{
   nir_deref_instr *deref = nir_src_as_deref(src);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   add_binding(state, var->data.descriptor_set, var->data.binding);
}

static void
add_tex_src_binding(struct apply_pipeline_layout_state *state,
                    nir_tex_instr *tex, nir_tex_src_type deref_src_type)
{
   int deref_src_idx = nir_tex_instr_src_index(tex, deref_src_type);
   if (deref_src_idx < 0)
      return;

   add_deref_src_binding(state, tex->src[deref_src_idx].src);
}

static bool
get_used_bindings(UNUSED nir_builder *_b, nir_instr *instr, void *_state)
{
   struct apply_pipeline_layout_state *state = _state;

   switch (instr->type) {
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_vulkan_resource_index:
         add_binding(state, nir_intrinsic_desc_set(intrin),
                     nir_intrinsic_binding(intrin));
         break;

      case nir_intrinsic_image_deref_load:
      case nir_intrinsic_image_deref_store:
      case nir_intrinsic_image_deref_atomic_add:
      case nir_intrinsic_image_deref_atomic_imin:
      case nir_intrinsic_image_deref_atomic_umin:
      case nir_intrinsic_image_deref_atomic_imax:
      case nir_intrinsic_image_deref_atomic_umax:
      case nir_intrinsic_image_deref_atomic_and:
      case nir_intrinsic_image_deref_atomic_or:
      case nir_intrinsic_image_deref_atomic_xor:
      case nir_intrinsic_image_deref_atomic_exchange:
      case nir_intrinsic_image_deref_atomic_comp_swap:
      case nir_intrinsic_image_deref_atomic_fadd:
      case nir_intrinsic_image_deref_size:
      case nir_intrinsic_image_deref_samples:
      case nir_intrinsic_image_deref_load_param_intel:
      case nir_intrinsic_image_deref_load_raw_intel:
      case nir_intrinsic_image_deref_store_raw_intel:
         add_deref_src_binding(state, intrin->src[0]);
         break;

      case nir_intrinsic_load_constant:
         state->uses_constants = true;
         break;

      default:
         break;
      }
      break;
   }
   case nir_instr_type_tex: {
      nir_tex_instr *tex = nir_instr_as_tex(instr);
      add_tex_src_binding(state, tex, nir_tex_src_texture_deref);
      add_tex_src_binding(state, tex, nir_tex_src_sampler_deref);
      break;
   }
   default:
      break;
   }

   return false;
}

static nir_intrinsic_instr *
find_descriptor_for_index_src(nir_src src,
                              struct apply_pipeline_layout_state *state)
{
   nir_intrinsic_instr *intrin = nir_src_as_intrinsic(src);

   while (intrin && intrin->intrinsic == nir_intrinsic_vulkan_resource_reindex)
      intrin = nir_src_as_intrinsic(intrin->src[0]);

   if (!intrin || intrin->intrinsic != nir_intrinsic_vulkan_resource_index)
      return NULL;

   return intrin;
}

static bool
descriptor_has_bti(nir_intrinsic_instr *intrin,
                   struct apply_pipeline_layout_state *state)
{
   assert(intrin->intrinsic == nir_intrinsic_vulkan_resource_index);

   uint32_t set = nir_intrinsic_desc_set(intrin);
   uint32_t binding = nir_intrinsic_binding(intrin);
   const struct anv_descriptor_set_binding_layout *bind_layout =
      &state->layout->set[set].layout->binding[binding];

   uint32_t surface_index;
   if (bind_layout->data & ANV_DESCRIPTOR_INLINE_UNIFORM)
      surface_index = state->set[set].desc_offset;
   else
      surface_index = state->set[set].surface_offsets[binding];

   /* Only lower to a BTI message if we have a valid binding table index. */
   return surface_index < MAX_BINDING_TABLE_SIZE;
}

static nir_address_format
descriptor_address_format(nir_intrinsic_instr *intrin,
                          struct apply_pipeline_layout_state *state)
{
   assert(intrin->intrinsic == nir_intrinsic_vulkan_resource_index);

   return addr_format_for_desc_type(nir_intrinsic_desc_type(intrin), state);
}

static nir_intrinsic_instr *
nir_deref_find_descriptor(nir_deref_instr *deref,
                          struct apply_pipeline_layout_state *state)
{
   while (1) {
      /* Nothing we will use this on has a variable */
      assert(deref->deref_type != nir_deref_type_var);

      nir_deref_instr *parent = nir_src_as_deref(deref->parent);
      if (!parent)
         break;

      deref = parent;
   }
   assert(deref->deref_type == nir_deref_type_cast);

   nir_intrinsic_instr *intrin = nir_src_as_intrinsic(deref->parent);
   if (!intrin || intrin->intrinsic != nir_intrinsic_load_vulkan_descriptor)
      return false;

   return find_descriptor_for_index_src(intrin->src[0], state);
}

static nir_ssa_def *
build_load_descriptor_mem(nir_builder *b,
                          nir_ssa_def *desc_addr, unsigned desc_offset,
                          unsigned num_components, unsigned bit_size,
                          struct apply_pipeline_layout_state *state)

{
   switch (state->desc_addr_format) {
   case nir_address_format_64bit_global_32bit_offset: {
      nir_ssa_def *base_addr =
         nir_pack_64_2x32(b, nir_channels(b, desc_addr, 0x3));
      nir_ssa_def *offset32 =
         nir_iadd_imm(b, nir_channel(b, desc_addr, 3), desc_offset);

      return nir_load_global_constant_offset(b, num_components, bit_size,
                                             base_addr, offset32,
                                             .align_mul = 8,
                                             .align_offset = desc_offset % 8);
   }

   case nir_address_format_32bit_index_offset: {
      nir_ssa_def *surface_index = nir_channel(b, desc_addr, 0);
      nir_ssa_def *offset32 =
         nir_iadd_imm(b, nir_channel(b, desc_addr, 1), desc_offset);

      return nir_load_ubo(b, num_components, bit_size,
                          surface_index, offset32,
                          .align_mul = 8,
                          .align_offset = desc_offset % 8,
                          .range_base = 0,
                          .range = ~0);
   }

   default:
      unreachable("Unsupported address format");
   }
}

/** Build a Vulkan resource index
 *
 * A "resource index" is the term used by our SPIR-V parser and the relevant
 * NIR intrinsics for a reference into a descriptor set.  It acts much like a
 * deref in NIR except that it accesses opaque descriptors instead of memory.
 *
 * Coming out of SPIR-V, both the resource indices (in the form of
 * vulkan_resource_[re]index intrinsics) and the memory derefs (in the form
 * of nir_deref_instr) use the same vector component/bit size.  The meaning
 * of those values for memory derefs (nir_deref_instr) is given by the
 * nir_address_format associated with the descriptor type.  For resource
 * indices, it's an entirely internal to ANV encoding which describes, in some
 * sense, the address of the descriptor.  Thanks to the NIR/SPIR-V rules, it
 * must be packed into the same size SSA values as a memory address.  For this
 * reason, the actual encoding may depend both on the address format for
 * memory derefs and the descriptor address format.
 *
 * The load_vulkan_descriptor intrinsic exists to provide a transition point
 * between these two forms of derefs: descriptor and memory.
 */
static nir_ssa_def *
build_res_index(nir_builder *b, uint32_t set, uint32_t binding,
                nir_ssa_def *array_index, nir_address_format addr_format,
                struct apply_pipeline_layout_state *state)
{
   const struct anv_descriptor_set_binding_layout *bind_layout =
      &state->layout->set[set].layout->binding[binding];

   uint32_t array_size = bind_layout->array_size;

   switch (addr_format) {
   case nir_address_format_64bit_global_32bit_offset:
   case nir_address_format_64bit_bounded_global: {
      uint32_t set_idx;
      switch (state->desc_addr_format) {
      case nir_address_format_64bit_global_32bit_offset:
         set_idx = set;
         break;

      case nir_address_format_32bit_index_offset:
         assert(state->set[set].desc_offset < MAX_BINDING_TABLE_SIZE);
         set_idx = state->set[set].desc_offset;
         break;

      default:
         unreachable("Unsupported address format");
      }

      assert(bind_layout->dynamic_offset_index < MAX_DYNAMIC_BUFFERS);
      uint32_t dynamic_offset_index = 0xff; /* No dynamic offset */
      if (bind_layout->dynamic_offset_index >= 0) {
         dynamic_offset_index =
            state->layout->set[set].dynamic_offset_start +
            bind_layout->dynamic_offset_index;
      }

      const uint32_t packed = (bind_layout->descriptor_stride << 16 ) | (set_idx << 8) | dynamic_offset_index;

      return nir_vec4(b, nir_imm_int(b, packed),
                         nir_imm_int(b, bind_layout->descriptor_offset),
                         nir_imm_int(b, array_size - 1),
                         array_index);
   }

   case nir_address_format_32bit_index_offset: {
      assert(state->desc_addr_format == nir_address_format_32bit_index_offset);
      if (bind_layout->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
         uint32_t surface_index = state->set[set].desc_offset;
         return nir_imm_ivec2(b, surface_index,
                                 bind_layout->descriptor_offset);
      } else {
         uint32_t surface_index = state->set[set].surface_offsets[binding];
         assert(array_size > 0 && array_size <= UINT16_MAX);
         assert(surface_index <= UINT16_MAX);
         uint32_t packed = ((array_size - 1) << 16) | surface_index;
         return nir_vec2(b, array_index, nir_imm_int(b, packed));
      }
   }

   default:
      unreachable("Unsupported address format");
   }
}

struct res_index_defs {
   nir_ssa_def *set_idx;
   nir_ssa_def *dyn_offset_base;
   nir_ssa_def *desc_offset_base;
   nir_ssa_def *array_index;
   nir_ssa_def *desc_stride;
};

static struct res_index_defs
unpack_res_index(nir_builder *b, nir_ssa_def *index)
{
   struct res_index_defs defs;

   nir_ssa_def *packed = nir_channel(b, index, 0);
   defs.desc_stride = nir_extract_u8(b, packed, nir_imm_int(b, 2));
   defs.set_idx = nir_extract_u8(b, packed, nir_imm_int(b, 1));
   defs.dyn_offset_base = nir_extract_u8(b, packed, nir_imm_int(b, 0));

   defs.desc_offset_base = nir_channel(b, index, 1);
   defs.array_index = nir_umin(b, nir_channel(b, index, 2),
                                  nir_channel(b, index, 3));

   return defs;
}

/** Adjust a Vulkan resource index
 *
 * This is the equivalent of nir_deref_type_ptr_as_array for resource indices.
 * For array descriptors, it allows us to adjust the array index.  Thanks to
 * variable pointers, we cannot always fold this re-index operation into the
 * vulkan_resource_index intrinsic and we have to do it based on nothing but
 * the address format.
 */
static nir_ssa_def *
build_res_reindex(nir_builder *b, nir_ssa_def *orig, nir_ssa_def *delta,
                  nir_address_format addr_format)
{
   switch (addr_format) {
   case nir_address_format_64bit_global_32bit_offset:
   case nir_address_format_64bit_bounded_global:
      return nir_vec4(b, nir_channel(b, orig, 0),
                         nir_channel(b, orig, 1),
                         nir_channel(b, orig, 2),
                         nir_iadd(b, nir_channel(b, orig, 3), delta));

   case nir_address_format_32bit_index_offset:
      return nir_vec2(b, nir_iadd(b, nir_channel(b, orig, 0), delta),
                         nir_channel(b, orig, 1));

   default:
      unreachable("Unhandled address format");
   }
}

/** Get the address for a descriptor given its resource index
 *
 * Because of the re-indexing operations, we can't bounds check descriptor
 * array access until we have the final index.  That means we end up doing the
 * bounds check here, if needed.  See unpack_res_index() for more details.
 *
 * This function takes both a bind_layout and a desc_type which are used to
 * determine the descriptor stride for array descriptors.  The bind_layout is
 * optional for buffer descriptor types.
 */
static nir_ssa_def *
build_desc_addr(nir_builder *b,
                const struct anv_descriptor_set_binding_layout *bind_layout,
                const VkDescriptorType desc_type,
                nir_ssa_def *index, nir_address_format addr_format,
                struct apply_pipeline_layout_state *state)
{
   switch (addr_format) {
   case nir_address_format_64bit_global_32bit_offset:
   case nir_address_format_64bit_bounded_global: {
      struct res_index_defs res = unpack_res_index(b, index);

      nir_ssa_def *desc_offset = res.desc_offset_base;
      if (desc_type != VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
         /* Compute the actual descriptor offset.  For inline uniform blocks,
          * the array index is ignored as they are only allowed to be a single
          * descriptor (not an array) and there is no concept of a "stride".
          *
          */
         desc_offset =
            nir_iadd(b, desc_offset, nir_imul(b, res.array_index, res.desc_stride));
      }

      switch (state->desc_addr_format) {
      case nir_address_format_64bit_global_32bit_offset: {
         nir_ssa_def *base_addr =
            nir_load_desc_set_address_intel(b, res.set_idx);
         return nir_vec4(b, nir_unpack_64_2x32_split_x(b, base_addr),
                            nir_unpack_64_2x32_split_y(b, base_addr),
                            nir_imm_int(b, UINT32_MAX),
                            desc_offset);
      }

      case nir_address_format_32bit_index_offset:
         return nir_vec2(b, res.set_idx, desc_offset);

      default:
         unreachable("Unhandled address format");
      }
   }

   case nir_address_format_32bit_index_offset:
      assert(desc_type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK);
      assert(state->desc_addr_format == nir_address_format_32bit_index_offset);
      return index;

   default:
      unreachable("Unhandled address format");
   }
}

/** Convert a Vulkan resource index into a buffer address
 *
 * In some cases, this does a  memory load from the descriptor set and, in
 * others, it simply converts from one form to another.
 *
 * See build_res_index for details about each resource index format.
 */
static nir_ssa_def *
build_buffer_addr_for_res_index(nir_builder *b,
                                const VkDescriptorType desc_type,
                                nir_ssa_def *res_index,
                                nir_address_format addr_format,
                                struct apply_pipeline_layout_state *state)
{
   if (desc_type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
      assert(addr_format == state->desc_addr_format);
      return build_desc_addr(b, NULL, desc_type, res_index, addr_format, state);
   } else if (addr_format == nir_address_format_32bit_index_offset) {
      nir_ssa_def *array_index = nir_channel(b, res_index, 0);
      nir_ssa_def *packed = nir_channel(b, res_index, 1);
      nir_ssa_def *array_max = nir_extract_u16(b, packed, nir_imm_int(b, 1));
      nir_ssa_def *surface_index = nir_extract_u16(b, packed, nir_imm_int(b, 0));

      if (state->add_bounds_checks)
         array_index = nir_umin(b, array_index, array_max);

      return nir_vec2(b, nir_iadd(b, surface_index, array_index),
                         nir_imm_int(b, 0));
   }

   nir_ssa_def *desc_addr =
      build_desc_addr(b, NULL, desc_type, res_index, addr_format, state);

   nir_ssa_def *desc = build_load_descriptor_mem(b, desc_addr, 0, 4, 32, state);

   if (state->has_dynamic_buffers) {
      struct res_index_defs res = unpack_res_index(b, res_index);

      /* This shader has dynamic offsets and we have no way of knowing
       * (save from the dynamic offset base index) if this buffer has a
       * dynamic offset.
       */
      nir_ssa_def *dyn_offset_idx =
         nir_iadd(b, res.dyn_offset_base, res.array_index);
      if (state->add_bounds_checks) {
         dyn_offset_idx = nir_umin(b, dyn_offset_idx,
                                      nir_imm_int(b, MAX_DYNAMIC_BUFFERS));
      }

      nir_ssa_def *dyn_load =
         nir_load_push_constant(b, 1, 32, nir_imul_imm(b, dyn_offset_idx, 4),
                                .base = offsetof(struct anv_push_constants, dynamic_offsets),
                                .range = MAX_DYNAMIC_BUFFERS * 4);

      nir_ssa_def *dynamic_offset =
         nir_bcsel(b, nir_ieq_imm(b, res.dyn_offset_base, 0xff),
                      nir_imm_int(b, 0), dyn_load);

      /* The dynamic offset gets added to the base pointer so that we
       * have a sliding window range.
       */
      nir_ssa_def *base_ptr =
         nir_pack_64_2x32(b, nir_channels(b, desc, 0x3));
      base_ptr = nir_iadd(b, base_ptr, nir_u2u64(b, dynamic_offset));
      desc = nir_vec4(b, nir_unpack_64_2x32_split_x(b, base_ptr),
                         nir_unpack_64_2x32_split_y(b, base_ptr),
                         nir_channel(b, desc, 2),
                         nir_channel(b, desc, 3));
   }

   /* The last element of the vec4 is always zero.
    *
    * See also struct anv_address_range_descriptor
    */
   return nir_vec4(b, nir_channel(b, desc, 0),
                      nir_channel(b, desc, 1),
                      nir_channel(b, desc, 2),
                      nir_imm_int(b, 0));
}

/** Loads descriptor memory for a variable-based deref chain
 *
 * The deref chain has to terminate at a variable with a descriptor_set and
 * binding set.  This is used for images, textures, and samplers.
 */
static nir_ssa_def *
build_load_var_deref_descriptor_mem(nir_builder *b, nir_deref_instr *deref,
                                    unsigned desc_offset,
                                    unsigned num_components, unsigned bit_size,
                                    struct apply_pipeline_layout_state *state)
{
   nir_variable *var = nir_deref_instr_get_variable(deref);

   const uint32_t set = var->data.descriptor_set;
   const uint32_t binding = var->data.binding;
   const struct anv_descriptor_set_binding_layout *bind_layout =
         &state->layout->set[set].layout->binding[binding];

   nir_ssa_def *array_index;
   if (deref->deref_type != nir_deref_type_var) {
      assert(deref->deref_type == nir_deref_type_array);
      assert(nir_deref_instr_parent(deref)->deref_type == nir_deref_type_var);
      assert(deref->arr.index.is_ssa);
      array_index = deref->arr.index.ssa;
   } else {
      array_index = nir_imm_int(b, 0);
   }

   /* It doesn't really matter what address format we choose as everything
    * will constant-fold nicely.  Choose one that uses the actual descriptor
    * buffer so we don't run into issues index/offset assumptions.
    */
   const nir_address_format addr_format =
      nir_address_format_64bit_bounded_global;

   nir_ssa_def *res_index =
      build_res_index(b, set, binding, array_index, addr_format, state);

   nir_ssa_def *desc_addr =
      build_desc_addr(b, bind_layout, bind_layout->type,
                      res_index, addr_format, state);

   return build_load_descriptor_mem(b, desc_addr, desc_offset,
                                    num_components, bit_size, state);
}

/** A recursive form of build_res_index()
 *
 * This recursively walks a resource [re]index chain and builds the resource
 * index.  It places the new code with the resource [re]index operation in the
 * hopes of better CSE.  This means the cursor is not where you left it when
 * this function returns.
 */
static nir_ssa_def *
build_res_index_for_chain(nir_builder *b, nir_intrinsic_instr *intrin,
                          nir_address_format addr_format,
                          uint32_t *set, uint32_t *binding,
                          struct apply_pipeline_layout_state *state)
{
   if (intrin->intrinsic == nir_intrinsic_vulkan_resource_index) {
      b->cursor = nir_before_instr(&intrin->instr);
      assert(intrin->src[0].is_ssa);
      *set = nir_intrinsic_desc_set(intrin);
      *binding = nir_intrinsic_binding(intrin);
      return build_res_index(b, *set, *binding, intrin->src[0].ssa,
                             addr_format, state);
   } else {
      assert(intrin->intrinsic == nir_intrinsic_vulkan_resource_reindex);
      nir_intrinsic_instr *parent = nir_src_as_intrinsic(intrin->src[0]);
      nir_ssa_def *index =
         build_res_index_for_chain(b, parent, addr_format,
                                   set, binding, state);

      b->cursor = nir_before_instr(&intrin->instr);

      assert(intrin->src[1].is_ssa);
      return build_res_reindex(b, index, intrin->src[1].ssa, addr_format);
   }
}

/** Builds a buffer address for a given vulkan [re]index intrinsic
 *
 * The cursor is not where you left it when this function returns.
 */
static nir_ssa_def *
build_buffer_addr_for_idx_intrin(nir_builder *b,
                                 nir_intrinsic_instr *idx_intrin,
                                 nir_address_format addr_format,
                                 struct apply_pipeline_layout_state *state)
{
   uint32_t set = UINT32_MAX, binding = UINT32_MAX;
   nir_ssa_def *res_index =
      build_res_index_for_chain(b, idx_intrin, addr_format,
                                &set, &binding, state);

   const struct anv_descriptor_set_binding_layout *bind_layout =
      &state->layout->set[set].layout->binding[binding];

   return build_buffer_addr_for_res_index(b, bind_layout->type,
                                          res_index, addr_format, state);
}

/** Builds a buffer address for deref chain
 *
 * This assumes that you can chase the chain all the way back to the original
 * vulkan_resource_index intrinsic.
 *
 * The cursor is not where you left it when this function returns.
 */
static nir_ssa_def *
build_buffer_addr_for_deref(nir_builder *b, nir_deref_instr *deref,
                            nir_address_format addr_format,
                            struct apply_pipeline_layout_state *state)
{
   nir_deref_instr *parent = nir_deref_instr_parent(deref);
   if (parent) {
      nir_ssa_def *addr =
         build_buffer_addr_for_deref(b, parent, addr_format, state);

      b->cursor = nir_before_instr(&deref->instr);
      return nir_explicit_io_address_from_deref(b, deref, addr, addr_format);
   }

   nir_intrinsic_instr *load_desc = nir_src_as_intrinsic(deref->parent);
   assert(load_desc->intrinsic == nir_intrinsic_load_vulkan_descriptor);

   nir_intrinsic_instr *idx_intrin = nir_src_as_intrinsic(load_desc->src[0]);

   b->cursor = nir_before_instr(&deref->instr);

   return build_buffer_addr_for_idx_intrin(b, idx_intrin, addr_format, state);
}

static bool
try_lower_direct_buffer_intrinsic(nir_builder *b,
                                  nir_intrinsic_instr *intrin, bool is_atomic,
                                  struct apply_pipeline_layout_state *state)
{
   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
   if (!nir_deref_mode_is_one_of(deref, nir_var_mem_ubo | nir_var_mem_ssbo))
      return false;

   nir_intrinsic_instr *desc = nir_deref_find_descriptor(deref, state);
   if (desc == NULL) {
      /* We should always be able to find the descriptor for UBO access. */
      assert(nir_deref_mode_is_one_of(deref, nir_var_mem_ssbo));
      return false;
   }

   nir_address_format addr_format = descriptor_address_format(desc, state);

   if (nir_deref_mode_is(deref, nir_var_mem_ssbo)) {
      /* 64-bit atomics only support A64 messages so we can't lower them to
       * the index+offset model.
       */
      if (is_atomic && nir_dest_bit_size(intrin->dest) == 64 &&
          !state->pdevice->info.has_lsc)
         return false;

      /* Normal binding table-based messages can't handle non-uniform access
       * so we have to fall back to A64.
       */
      if (nir_intrinsic_access(intrin) & ACCESS_NON_UNIFORM)
         return false;

      if (!descriptor_has_bti(desc, state))
         return false;

      /* Rewrite to 32bit_index_offset whenever we can */
      addr_format = nir_address_format_32bit_index_offset;
   } else {
      assert(nir_deref_mode_is(deref, nir_var_mem_ubo));

      /* Rewrite to 32bit_index_offset whenever we can */
      if (descriptor_has_bti(desc, state))
         addr_format = nir_address_format_32bit_index_offset;
   }

   nir_ssa_def *addr =
      build_buffer_addr_for_deref(b, deref, addr_format, state);

   b->cursor = nir_before_instr(&intrin->instr);
   nir_lower_explicit_io_instr(b, intrin, addr, addr_format);

   return true;
}

static bool
lower_load_accel_struct_desc(nir_builder *b,
                             nir_intrinsic_instr *load_desc,
                             struct apply_pipeline_layout_state *state)
{
   assert(load_desc->intrinsic == nir_intrinsic_load_vulkan_descriptor);

   nir_intrinsic_instr *idx_intrin = nir_src_as_intrinsic(load_desc->src[0]);

   /* It doesn't really matter what address format we choose as
    * everything will constant-fold nicely.  Choose one that uses the
    * actual descriptor buffer.
    */
   const nir_address_format addr_format =
      nir_address_format_64bit_bounded_global;

   uint32_t set = UINT32_MAX, binding = UINT32_MAX;
   nir_ssa_def *res_index =
      build_res_index_for_chain(b, idx_intrin, addr_format,
                                &set, &binding, state);

   const struct anv_descriptor_set_binding_layout *bind_layout =
      &state->layout->set[set].layout->binding[binding];

   b->cursor = nir_before_instr(&load_desc->instr);

   nir_ssa_def *desc_addr =
      build_desc_addr(b, bind_layout, bind_layout->type,
                      res_index, addr_format, state);

   /* Acceleration structure descriptors are always uint64_t */
   nir_ssa_def *desc = build_load_descriptor_mem(b, desc_addr, 0, 1, 64, state);

   assert(load_desc->dest.is_ssa);
   assert(load_desc->dest.ssa.bit_size == 64);
   assert(load_desc->dest.ssa.num_components == 1);
   nir_ssa_def_rewrite_uses(&load_desc->dest.ssa, desc);
   nir_instr_remove(&load_desc->instr);

   return true;
}

static bool
lower_direct_buffer_instr(nir_builder *b, nir_instr *instr, void *_state)
{
   struct apply_pipeline_layout_state *state = _state;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_deref:
   case nir_intrinsic_store_deref:
      return try_lower_direct_buffer_intrinsic(b, intrin, false, state);

   case nir_intrinsic_deref_atomic_add:
   case nir_intrinsic_deref_atomic_imin:
   case nir_intrinsic_deref_atomic_umin:
   case nir_intrinsic_deref_atomic_imax:
   case nir_intrinsic_deref_atomic_umax:
   case nir_intrinsic_deref_atomic_and:
   case nir_intrinsic_deref_atomic_or:
   case nir_intrinsic_deref_atomic_xor:
   case nir_intrinsic_deref_atomic_exchange:
   case nir_intrinsic_deref_atomic_comp_swap:
   case nir_intrinsic_deref_atomic_fadd:
   case nir_intrinsic_deref_atomic_fmin:
   case nir_intrinsic_deref_atomic_fmax:
   case nir_intrinsic_deref_atomic_fcomp_swap:
      return try_lower_direct_buffer_intrinsic(b, intrin, true, state);

   case nir_intrinsic_get_ssbo_size: {
      /* The get_ssbo_size intrinsic always just takes a
       * index/reindex intrinsic.
       */
      nir_intrinsic_instr *idx_intrin =
         find_descriptor_for_index_src(intrin->src[0], state);
      if (idx_intrin == NULL || !descriptor_has_bti(idx_intrin, state))
         return false;

      b->cursor = nir_before_instr(&intrin->instr);

      /* We just checked that this is a BTI descriptor */
      const nir_address_format addr_format =
         nir_address_format_32bit_index_offset;

      nir_ssa_def *buffer_addr =
         build_buffer_addr_for_idx_intrin(b, idx_intrin, addr_format, state);

      b->cursor = nir_before_instr(&intrin->instr);
      nir_ssa_def *bti = nir_channel(b, buffer_addr, 0);

      nir_instr_rewrite_src(&intrin->instr, &intrin->src[0],
                            nir_src_for_ssa(bti));
      _mesa_set_add(state->lowered_instrs, intrin);
      return true;
   }

   case nir_intrinsic_load_vulkan_descriptor:
      if (nir_intrinsic_desc_type(intrin) ==
          VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)
         return lower_load_accel_struct_desc(b, intrin, state);
      return false;

   default:
      return false;
   }
}

static bool
lower_res_index_intrinsic(nir_builder *b, nir_intrinsic_instr *intrin,
                          struct apply_pipeline_layout_state *state)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_address_format addr_format =
      addr_format_for_desc_type(nir_intrinsic_desc_type(intrin), state);

   assert(intrin->src[0].is_ssa);
   nir_ssa_def *index =
      build_res_index(b, nir_intrinsic_desc_set(intrin),
                         nir_intrinsic_binding(intrin),
                         intrin->src[0].ssa,
                         addr_format, state);

   assert(intrin->dest.is_ssa);
   assert(intrin->dest.ssa.bit_size == index->bit_size);
   assert(intrin->dest.ssa.num_components == index->num_components);
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, index);
   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_res_reindex_intrinsic(nir_builder *b, nir_intrinsic_instr *intrin,
                            struct apply_pipeline_layout_state *state)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_address_format addr_format =
      addr_format_for_desc_type(nir_intrinsic_desc_type(intrin), state);

   assert(intrin->src[0].is_ssa && intrin->src[1].is_ssa);
   nir_ssa_def *index =
      build_res_reindex(b, intrin->src[0].ssa,
                           intrin->src[1].ssa,
                           addr_format);

   assert(intrin->dest.is_ssa);
   assert(intrin->dest.ssa.bit_size == index->bit_size);
   assert(intrin->dest.ssa.num_components == index->num_components);
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, index);
   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_load_vulkan_descriptor(nir_builder *b, nir_intrinsic_instr *intrin,
                             struct apply_pipeline_layout_state *state)
{
   b->cursor = nir_before_instr(&intrin->instr);

   const VkDescriptorType desc_type = nir_intrinsic_desc_type(intrin);
   nir_address_format addr_format = addr_format_for_desc_type(desc_type, state);

   assert(intrin->dest.is_ssa);
   nir_foreach_use(src, &intrin->dest.ssa) {
      if (src->parent_instr->type != nir_instr_type_deref)
         continue;

      nir_deref_instr *cast = nir_instr_as_deref(src->parent_instr);
      assert(cast->deref_type == nir_deref_type_cast);
      switch (desc_type) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         cast->cast.align_mul = ANV_UBO_ALIGNMENT;
         cast->cast.align_offset = 0;
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         cast->cast.align_mul = ANV_SSBO_ALIGNMENT;
         cast->cast.align_offset = 0;
         break;

      default:
         break;
      }
   }

   assert(intrin->src[0].is_ssa);
   nir_ssa_def *desc =
      build_buffer_addr_for_res_index(b, desc_type, intrin->src[0].ssa,
                                      addr_format, state);

   assert(intrin->dest.is_ssa);
   assert(intrin->dest.ssa.bit_size == desc->bit_size);
   assert(intrin->dest.ssa.num_components == desc->num_components);
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, desc);
   nir_instr_remove(&intrin->instr);

   return true;
}

static bool
lower_get_ssbo_size(nir_builder *b, nir_intrinsic_instr *intrin,
                    struct apply_pipeline_layout_state *state)
{
   if (_mesa_set_search(state->lowered_instrs, intrin))
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_address_format addr_format =
      addr_format_for_desc_type(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, state);

   assert(intrin->src[0].is_ssa);
   nir_ssa_def *desc =
      build_buffer_addr_for_res_index(b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      intrin->src[0].ssa, addr_format, state);

   switch (addr_format) {
   case nir_address_format_64bit_global_32bit_offset:
   case nir_address_format_64bit_bounded_global: {
      nir_ssa_def *size = nir_channel(b, desc, 2);
      nir_ssa_def_rewrite_uses(&intrin->dest.ssa, size);
      nir_instr_remove(&intrin->instr);
      break;
   }

   case nir_address_format_32bit_index_offset:
      /* The binding table index is the first component of the address.  The
       * back-end wants a scalar binding table index source.
       */
      nir_instr_rewrite_src(&intrin->instr, &intrin->src[0],
                            nir_src_for_ssa(nir_channel(b, desc, 0)));
      break;

   default:
      unreachable("Unsupported address format");
   }

   return true;
}

static bool
image_binding_needs_lowered_surface(nir_variable *var)
{
   return !(var->data.access & ACCESS_NON_READABLE) &&
          var->data.image.format != PIPE_FORMAT_NONE;
}

static bool
lower_image_intrinsic(nir_builder *b, nir_intrinsic_instr *intrin,
                      struct apply_pipeline_layout_state *state)
{
   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);

   unsigned set = var->data.descriptor_set;
   unsigned binding = var->data.binding;
   unsigned binding_offset = state->set[set].surface_offsets[binding];

   b->cursor = nir_before_instr(&intrin->instr);

   if (binding_offset > MAX_BINDING_TABLE_SIZE) {
      const unsigned desc_comp =
         image_binding_needs_lowered_surface(var) ? 1 : 0;
      nir_ssa_def *desc =
         build_load_var_deref_descriptor_mem(b, deref, 0, 2, 32, state);
      nir_ssa_def *handle = nir_channel(b, desc, desc_comp);
      nir_rewrite_image_intrinsic(intrin, handle, true);
   } else {
      unsigned array_size =
         state->layout->set[set].layout->binding[binding].array_size;

      nir_ssa_def *index = NULL;
      if (deref->deref_type != nir_deref_type_var) {
         assert(deref->deref_type == nir_deref_type_array);
         index = nir_ssa_for_src(b, deref->arr.index, 1);
         if (state->add_bounds_checks)
            index = nir_umin(b, index, nir_imm_int(b, array_size - 1));
      } else {
         index = nir_imm_int(b, 0);
      }

      index = nir_iadd_imm(b, index, binding_offset);
      nir_rewrite_image_intrinsic(intrin, index, false);
   }

   return true;
}

static bool
lower_load_constant(nir_builder *b, nir_intrinsic_instr *intrin,
                    struct apply_pipeline_layout_state *state)
{
   b->cursor = nir_instr_remove(&intrin->instr);

   /* Any constant-offset load_constant instructions should have been removed
    * by constant folding.
    */
   assert(!nir_src_is_const(intrin->src[0]));
   nir_ssa_def *offset = nir_iadd_imm(b, nir_ssa_for_src(b, intrin->src[0], 1),
                                      nir_intrinsic_base(intrin));

   unsigned load_size = intrin->dest.ssa.num_components *
                        intrin->dest.ssa.bit_size / 8;
   unsigned load_align = intrin->dest.ssa.bit_size / 8;

   assert(load_size < b->shader->constant_data_size);
   unsigned max_offset = b->shader->constant_data_size - load_size;
   offset = nir_umin(b, offset, nir_imm_int(b, max_offset));

   nir_ssa_def *const_data_base_addr = nir_pack_64_2x32_split(b,
      nir_load_reloc_const_intel(b, BRW_SHADER_RELOC_CONST_DATA_ADDR_LOW),
      nir_load_reloc_const_intel(b, BRW_SHADER_RELOC_CONST_DATA_ADDR_HIGH));

   nir_ssa_def *data =
      nir_load_global_constant(b, nir_iadd(b, const_data_base_addr,
                                              nir_u2u64(b, offset)),
                               load_align,
                               intrin->dest.ssa.num_components,
                               intrin->dest.ssa.bit_size);

   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, data);

   return true;
}

static bool
lower_base_workgroup_id(nir_builder *b, nir_intrinsic_instr *intrin,
                        struct apply_pipeline_layout_state *state)
{
   b->cursor = nir_instr_remove(&intrin->instr);

   nir_ssa_def *base_workgroup_id =
      nir_load_push_constant(b, 3, 32, nir_imm_int(b, 0),
                             .base = offsetof(struct anv_push_constants, cs.base_work_group_id),
                             .range = 3 * sizeof(uint32_t));
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, base_workgroup_id);

   return true;
}

static void
lower_tex_deref(nir_builder *b, nir_tex_instr *tex,
                nir_tex_src_type deref_src_type,
                unsigned *base_index, unsigned plane,
                struct apply_pipeline_layout_state *state)
{
   int deref_src_idx = nir_tex_instr_src_index(tex, deref_src_type);
   if (deref_src_idx < 0)
      return;

   nir_deref_instr *deref = nir_src_as_deref(tex->src[deref_src_idx].src);
   nir_variable *var = nir_deref_instr_get_variable(deref);

   unsigned set = var->data.descriptor_set;
   unsigned binding = var->data.binding;
   unsigned array_size =
      state->layout->set[set].layout->binding[binding].array_size;

   unsigned binding_offset;
   if (deref_src_type == nir_tex_src_texture_deref) {
      binding_offset = state->set[set].surface_offsets[binding];
   } else {
      assert(deref_src_type == nir_tex_src_sampler_deref);
      binding_offset = state->set[set].sampler_offsets[binding];
   }

   nir_tex_src_type offset_src_type;
   nir_ssa_def *index = NULL;
   if (binding_offset > MAX_BINDING_TABLE_SIZE) {
      const unsigned plane_offset =
         plane * sizeof(struct anv_sampled_image_descriptor);

      nir_ssa_def *desc =
         build_load_var_deref_descriptor_mem(b, deref, plane_offset,
                                             2, 32, state);

      if (deref_src_type == nir_tex_src_texture_deref) {
         offset_src_type = nir_tex_src_texture_handle;
         index = nir_channel(b, desc, 0);
      } else {
         assert(deref_src_type == nir_tex_src_sampler_deref);
         offset_src_type = nir_tex_src_sampler_handle;
         index = nir_channel(b, desc, 1);
      }
   } else {
      if (deref_src_type == nir_tex_src_texture_deref) {
         offset_src_type = nir_tex_src_texture_offset;
      } else {
         assert(deref_src_type == nir_tex_src_sampler_deref);
         offset_src_type = nir_tex_src_sampler_offset;
      }

      *base_index = binding_offset + plane;

      if (deref->deref_type != nir_deref_type_var) {
         assert(deref->deref_type == nir_deref_type_array);

         if (nir_src_is_const(deref->arr.index)) {
            unsigned arr_index = MIN2(nir_src_as_uint(deref->arr.index), array_size - 1);
            struct anv_sampler **immutable_samplers =
               state->layout->set[set].layout->binding[binding].immutable_samplers;
            if (immutable_samplers) {
               /* Array of YCbCr samplers are tightly packed in the binding
                * tables, compute the offset of an element in the array by
                * adding the number of planes of all preceding elements.
                */
               unsigned desc_arr_index = 0;
               for (int i = 0; i < arr_index; i++)
                  desc_arr_index += immutable_samplers[i]->n_planes;
               *base_index += desc_arr_index;
            } else {
               *base_index += arr_index;
            }
         } else {
            /* From VK_KHR_sampler_ycbcr_conversion:
             *
             * If sampler Y’CBCR conversion is enabled, the combined image
             * sampler must be indexed only by constant integral expressions
             * when aggregated into arrays in shader code, irrespective of
             * the shaderSampledImageArrayDynamicIndexing feature.
             */
            assert(nir_tex_instr_src_index(tex, nir_tex_src_plane) == -1);

            index = nir_ssa_for_src(b, deref->arr.index, 1);

            if (state->add_bounds_checks)
               index = nir_umin(b, index, nir_imm_int(b, array_size - 1));
         }
      }
   }

   if (index) {
      nir_instr_rewrite_src(&tex->instr, &tex->src[deref_src_idx].src,
                            nir_src_for_ssa(index));
      tex->src[deref_src_idx].src_type = offset_src_type;
   } else {
      nir_tex_instr_remove_src(tex, deref_src_idx);
   }
}

static uint32_t
tex_instr_get_and_remove_plane_src(nir_tex_instr *tex)
{
   int plane_src_idx = nir_tex_instr_src_index(tex, nir_tex_src_plane);
   if (plane_src_idx < 0)
      return 0;

   unsigned plane = nir_src_as_uint(tex->src[plane_src_idx].src);

   nir_tex_instr_remove_src(tex, plane_src_idx);

   return plane;
}

static nir_ssa_def *
build_def_array_select(nir_builder *b, nir_ssa_def **srcs, nir_ssa_def *idx,
                       unsigned start, unsigned end)
{
   if (start == end - 1) {
      return srcs[start];
   } else {
      unsigned mid = start + (end - start) / 2;
      return nir_bcsel(b, nir_ilt(b, idx, nir_imm_int(b, mid)),
                       build_def_array_select(b, srcs, idx, start, mid),
                       build_def_array_select(b, srcs, idx, mid, end));
   }
}

static bool
lower_tex(nir_builder *b, nir_tex_instr *tex,
          struct apply_pipeline_layout_state *state)
{
   unsigned plane = tex_instr_get_and_remove_plane_src(tex);

   b->cursor = nir_before_instr(&tex->instr);

   lower_tex_deref(b, tex, nir_tex_src_texture_deref,
                   &tex->texture_index, plane, state);

   lower_tex_deref(b, tex, nir_tex_src_sampler_deref,
                   &tex->sampler_index, plane, state);

   return true;
}

static bool
lower_ray_query_globals(nir_builder *b, nir_intrinsic_instr *intrin,
                        struct apply_pipeline_layout_state *state)
{
   b->cursor = nir_instr_remove(&intrin->instr);

   nir_ssa_def *rq_globals =
      nir_load_push_constant(b, 1, 64, nir_imm_int(b, 0),
                             .base = offsetof(struct anv_push_constants, ray_query_globals),
                             .range = sizeof_field(struct anv_push_constants, ray_query_globals));
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, rq_globals);

   return true;
}

static bool
apply_pipeline_layout(nir_builder *b, nir_instr *instr, void *_state)
{
   struct apply_pipeline_layout_state *state = _state;

   switch (instr->type) {
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_vulkan_resource_index:
         return lower_res_index_intrinsic(b, intrin, state);
      case nir_intrinsic_vulkan_resource_reindex:
         return lower_res_reindex_intrinsic(b, intrin, state);
      case nir_intrinsic_load_vulkan_descriptor:
         return lower_load_vulkan_descriptor(b, intrin, state);
      case nir_intrinsic_get_ssbo_size:
         return lower_get_ssbo_size(b, intrin, state);
      case nir_intrinsic_image_deref_load:
      case nir_intrinsic_image_deref_store:
      case nir_intrinsic_image_deref_atomic_add:
      case nir_intrinsic_image_deref_atomic_imin:
      case nir_intrinsic_image_deref_atomic_umin:
      case nir_intrinsic_image_deref_atomic_imax:
      case nir_intrinsic_image_deref_atomic_umax:
      case nir_intrinsic_image_deref_atomic_and:
      case nir_intrinsic_image_deref_atomic_or:
      case nir_intrinsic_image_deref_atomic_xor:
      case nir_intrinsic_image_deref_atomic_exchange:
      case nir_intrinsic_image_deref_atomic_comp_swap:
      case nir_intrinsic_image_deref_atomic_fadd:
      case nir_intrinsic_image_deref_size:
      case nir_intrinsic_image_deref_samples:
      case nir_intrinsic_image_deref_load_param_intel:
      case nir_intrinsic_image_deref_load_raw_intel:
      case nir_intrinsic_image_deref_store_raw_intel:
         return lower_image_intrinsic(b, intrin, state);
      case nir_intrinsic_load_constant:
         return lower_load_constant(b, intrin, state);
      case nir_intrinsic_load_base_workgroup_id:
         return lower_base_workgroup_id(b, intrin, state);
      case nir_intrinsic_load_ray_query_global_intel:
         return lower_ray_query_globals(b, intrin, state);
      default:
         return false;
      }
      break;
   }
   case nir_instr_type_tex:
      return lower_tex(b, nir_instr_as_tex(instr), state);
   default:
      return false;
   }
}

struct binding_info {
   uint32_t binding;
   uint8_t set;
   uint16_t score;
};

static int
compare_binding_infos(const void *_a, const void *_b)
{
   const struct binding_info *a = _a, *b = _b;
   if (a->score != b->score)
      return b->score - a->score;

   if (a->set != b->set)
      return a->set - b->set;

   return a->binding - b->binding;
}

void
anv_nir_apply_pipeline_layout(nir_shader *shader,
                              const struct anv_physical_device *pdevice,
                              bool robust_buffer_access,
                              const struct anv_pipeline_layout *layout,
                              struct anv_pipeline_bind_map *map)
{
   void *mem_ctx = ralloc_context(NULL);

   struct apply_pipeline_layout_state state = {
      .pdevice = pdevice,
      .layout = layout,
      .add_bounds_checks = robust_buffer_access,
      .desc_addr_format =
            brw_shader_stage_requires_bindless_resources(shader->info.stage) ?
                          nir_address_format_64bit_global_32bit_offset :
                          nir_address_format_32bit_index_offset,
      .ssbo_addr_format = anv_nir_ssbo_addr_format(pdevice, robust_buffer_access),
      .ubo_addr_format = anv_nir_ubo_addr_format(pdevice, robust_buffer_access),
      .lowered_instrs = _mesa_pointer_set_create(mem_ctx),
   };

   for (unsigned s = 0; s < layout->num_sets; s++) {
      const unsigned count = layout->set[s].layout->binding_count;
      state.set[s].use_count = rzalloc_array(mem_ctx, uint8_t, count);
      state.set[s].surface_offsets = rzalloc_array(mem_ctx, uint8_t, count);
      state.set[s].sampler_offsets = rzalloc_array(mem_ctx, uint8_t, count);
   }

   nir_shader_instructions_pass(shader, get_used_bindings,
                                nir_metadata_all, &state);

   for (unsigned s = 0; s < layout->num_sets; s++) {
      if (state.desc_addr_format != nir_address_format_32bit_index_offset) {
         state.set[s].desc_offset = BINDLESS_OFFSET;
      } else if (state.set[s].desc_buffer_used) {
         map->surface_to_descriptor[map->surface_count] =
            (struct anv_pipeline_binding) {
               .set = ANV_DESCRIPTOR_SET_DESCRIPTORS,
               .binding = UINT32_MAX,
               .index = s,
            };
         state.set[s].desc_offset = map->surface_count;
         map->surface_count++;
      }
   }

   unsigned used_binding_count = 0;
   for (uint32_t set = 0; set < layout->num_sets; set++) {
      struct anv_descriptor_set_layout *set_layout = layout->set[set].layout;
      for (unsigned b = 0; b < set_layout->binding_count; b++) {
         if (state.set[set].use_count[b] == 0)
            continue;

         used_binding_count++;
      }
   }

   struct binding_info *infos =
      rzalloc_array(mem_ctx, struct binding_info, used_binding_count);
   used_binding_count = 0;
   for (uint32_t set = 0; set < layout->num_sets; set++) {
      const struct anv_descriptor_set_layout *set_layout = layout->set[set].layout;
      for (unsigned b = 0; b < set_layout->binding_count; b++) {
         if (state.set[set].use_count[b] == 0)
            continue;

         const struct anv_descriptor_set_binding_layout *binding =
               &layout->set[set].layout->binding[b];

         /* Do a fixed-point calculation to generate a score based on the
          * number of uses and the binding array size.  We shift by 7 instead
          * of 8 because we're going to use the top bit below to make
          * everything which does not support bindless super higher priority
          * than things which do.
          */
         uint16_t score = ((uint16_t)state.set[set].use_count[b] << 7) /
                          binding->array_size;

         /* If the descriptor type doesn't support bindless then put it at the
          * beginning so we guarantee it gets a slot.
          */
         if (!anv_descriptor_supports_bindless(pdevice, binding, true) ||
             !anv_descriptor_supports_bindless(pdevice, binding, false))
            score |= 1 << 15;

         infos[used_binding_count++] = (struct binding_info) {
            .set = set,
            .binding = b,
            .score = score,
         };
      }
   }

   /* Order the binding infos based on score with highest scores first.  If
    * scores are equal we then order by set and binding.
    */
   qsort(infos, used_binding_count, sizeof(struct binding_info),
         compare_binding_infos);

   for (unsigned i = 0; i < used_binding_count; i++) {
      unsigned set = infos[i].set, b = infos[i].binding;
      const struct anv_descriptor_set_binding_layout *binding =
            &layout->set[set].layout->binding[b];

      const uint32_t array_size = binding->array_size;

      if (binding->dynamic_offset_index >= 0)
         state.has_dynamic_buffers = true;

      if (binding->data & ANV_DESCRIPTOR_SURFACE_STATE) {
         if (map->surface_count + array_size > MAX_BINDING_TABLE_SIZE ||
             anv_descriptor_requires_bindless(pdevice, binding, false) ||
             brw_shader_stage_requires_bindless_resources(shader->info.stage)) {
            /* If this descriptor doesn't fit in the binding table or if it
             * requires bindless for some reason, flag it as bindless.
             */
            assert(anv_descriptor_supports_bindless(pdevice, binding, false));
            state.set[set].surface_offsets[b] = BINDLESS_OFFSET;
         } else {
            state.set[set].surface_offsets[b] = map->surface_count;
            if (binding->dynamic_offset_index < 0) {
               struct anv_sampler **samplers = binding->immutable_samplers;
               for (unsigned i = 0; i < binding->array_size; i++) {
                  uint8_t planes = samplers ? samplers[i]->n_planes : 1;
                  for (uint8_t p = 0; p < planes; p++) {
                     map->surface_to_descriptor[map->surface_count++] =
                        (struct anv_pipeline_binding) {
                           .set = set,
                           .binding = b,
                           .index = binding->descriptor_index + i,
                           .plane = p,
                        };
                  }
               }
            } else {
               for (unsigned i = 0; i < binding->array_size; i++) {
                  map->surface_to_descriptor[map->surface_count++] =
                     (struct anv_pipeline_binding) {
                        .set = set,
                        .binding = b,
                        .index = binding->descriptor_index + i,
                        .dynamic_offset_index =
                           layout->set[set].dynamic_offset_start +
                           binding->dynamic_offset_index + i,
                     };
               }
            }
         }
         assert(map->surface_count <= MAX_BINDING_TABLE_SIZE);
      }

      if (binding->data & ANV_DESCRIPTOR_SAMPLER_STATE) {
         if (map->sampler_count + array_size > MAX_SAMPLER_TABLE_SIZE ||
             anv_descriptor_requires_bindless(pdevice, binding, true) ||
             brw_shader_stage_requires_bindless_resources(shader->info.stage)) {
            /* If this descriptor doesn't fit in the binding table or if it
             * requires bindless for some reason, flag it as bindless.
             *
             * We also make large sampler arrays bindless because we can avoid
             * using indirect sends thanks to bindless samplers being packed
             * less tightly than the sampler table.
             */
            assert(anv_descriptor_supports_bindless(pdevice, binding, true));
            state.set[set].sampler_offsets[b] = BINDLESS_OFFSET;
         } else {
            state.set[set].sampler_offsets[b] = map->sampler_count;
            struct anv_sampler **samplers = binding->immutable_samplers;
            for (unsigned i = 0; i < binding->array_size; i++) {
               uint8_t planes = samplers ? samplers[i]->n_planes : 1;
               for (uint8_t p = 0; p < planes; p++) {
                  map->sampler_to_descriptor[map->sampler_count++] =
                     (struct anv_pipeline_binding) {
                        .set = set,
                        .binding = b,
                        .index = binding->descriptor_index + i,
                        .plane = p,
                     };
               }
            }
         }
      }
   }

   nir_foreach_image_variable(var, shader) {
      const uint32_t set = var->data.descriptor_set;
      const uint32_t binding = var->data.binding;
      const struct anv_descriptor_set_binding_layout *bind_layout =
            &layout->set[set].layout->binding[binding];
      const uint32_t array_size = bind_layout->array_size;

      if (state.set[set].use_count[binding] == 0)
         continue;

      if (state.set[set].surface_offsets[binding] >= MAX_BINDING_TABLE_SIZE)
         continue;

      struct anv_pipeline_binding *pipe_binding =
         &map->surface_to_descriptor[state.set[set].surface_offsets[binding]];
      for (unsigned i = 0; i < array_size; i++) {
         assert(pipe_binding[i].set == set);
         assert(pipe_binding[i].index == bind_layout->descriptor_index + i);

         pipe_binding[i].lowered_storage_surface =
            image_binding_needs_lowered_surface(var);
      }
   }

   /* Before we do the normal lowering, we look for any SSBO operations
    * that we can lower to the BTI model and lower them up-front.  The BTI
    * model can perform better than the A64 model for a couple reasons:
    *
    *  1. 48-bit address calculations are potentially expensive and using
    *     the BTI model lets us simply compute 32-bit offsets and the
    *     hardware adds the 64-bit surface base address.
    *
    *  2. The BTI messages, because they use surface states, do bounds
    *     checking for us.  With the A64 model, we have to do our own
    *     bounds checking and this means wider pointers and extra
    *     calculations and branching in the shader.
    *
    * The solution to both of these is to convert things to the BTI model
    * opportunistically.  The reason why we need to do this as a pre-pass
    * is for two reasons:
    *
    *  1. The BTI model requires nir_address_format_32bit_index_offset
    *     pointers which are not the same type as the pointers needed for
    *     the A64 model.  Because all our derefs are set up for the A64
    *     model (in case we have variable pointers), we have to crawl all
    *     the way back to the vulkan_resource_index intrinsic and build a
    *     completely fresh index+offset calculation.
    *
    *  2. Because the variable-pointers-capable lowering that we do as part
    *     of apply_pipeline_layout_block is destructive (It really has to
    *     be to handle variable pointers properly), we've lost the deref
    *     information by the time we get to the load/store/atomic
    *     intrinsics in that pass.
    */
   nir_shader_instructions_pass(shader, lower_direct_buffer_instr,
                                nir_metadata_block_index |
                                nir_metadata_dominance,
                                &state);

   /* We just got rid of all the direct access.  Delete it so it's not in the
    * way when we do our indirect lowering.
    */
   nir_opt_dce(shader);

   nir_shader_instructions_pass(shader, apply_pipeline_layout,
                                nir_metadata_block_index |
                                nir_metadata_dominance,
                                &state);

   ralloc_free(mem_ctx);

   if (brw_shader_stage_is_bindless(shader->info.stage)) {
      assert(map->surface_count == 0);
      assert(map->sampler_count == 0);
   }

   /* Now that we're done computing the surface and sampler portions of the
    * bind map, hash them.  This lets us quickly determine if the actual
    * mapping has changed and not just a no-op pipeline change.
    */
   _mesa_sha1_compute(map->surface_to_descriptor,
                      map->surface_count * sizeof(struct anv_pipeline_binding),
                      map->surface_sha1);
   _mesa_sha1_compute(map->sampler_to_descriptor,
                      map->sampler_count * sizeof(struct anv_pipeline_binding),
                      map->sampler_sha1);
}
