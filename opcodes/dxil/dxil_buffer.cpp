/*
 * Copyright 2019-2020 Hans-Kristian Arntzen for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "dxil_buffer.hpp"
#include "dxil_common.hpp"
#include "dxil_sampling.hpp"
#include "logging.hpp"
#include "opcodes/converter_impl.hpp"

namespace dxil_spv
{
BufferAccessInfo build_buffer_access(Converter::Impl &impl, const llvm::CallInst *instruction, unsigned operand_offset)
{
	auto &builder = impl.builder();
	spv::Id image_id = impl.get_id_for_value(instruction->getOperand(1));
	const auto &meta = impl.handle_to_resource_meta[image_id];

	spv::Id index_id = impl.get_id_for_value(instruction->getOperand(2 + operand_offset));
	unsigned num_components = 4;

	if (meta.kind == DXIL::ResourceKind::RawBuffer)
	{
		// For raw buffers, the index is in bytes. Since we only consider bytes, shift by 4.
		Operation *op = impl.allocate(spv::OpShiftRightLogical, builder.makeUintType(32));
		op->add_ids({ index_id, builder.makeUintConstant(2) });
		index_id = op->id;
		impl.add(op);
	}
	else if (meta.kind == DXIL::ResourceKind::StructuredBuffer)
	{
		unsigned constant_offset = 0;
		spv::Id offset_id = impl.get_id_for_value(instruction->getOperand(3 + operand_offset));
		bool has_constant_offset = false;
		if (llvm::isa<llvm::ConstantInt>(instruction->getOperand(3 + operand_offset)))
		{
			constant_offset = unsigned(llvm::cast<llvm::ConstantInt>(instruction->getOperand(3 + operand_offset))
			                               ->getUniqueInteger()
			                               .getZExtValue());
			has_constant_offset = true;
		}

		num_components = std::min(4u, (meta.stride - constant_offset) / 4);

		if (meta.stride != 4)
		{
			Operation *op = impl.allocate(spv::OpIMul, builder.makeUintType(32));
			op->add_ids({ index_id, builder.makeUintConstant(meta.stride / 4) });
			index_id = op->id;
			impl.add(op);
		}

		if (has_constant_offset)
		{
			if (constant_offset != 0)
			{
				Operation *op = impl.allocate(spv::OpIAdd, builder.makeUintType(32));
				op->add_ids({ index_id, builder.makeUintConstant(constant_offset / 4) });
				index_id = op->id;
				impl.add(op);
			}
		}
		else
		{
			// Dynamically offset into the structured element.
			Operation *op = impl.allocate(spv::OpShiftRightLogical, builder.makeUintType(32));
			op->add_ids({ offset_id, builder.makeUintConstant(2) });
			offset_id = op->id;
			impl.add(op);

			op = impl.allocate(spv::OpIAdd, builder.makeUintType(32));
			op->add_ids({ index_id, offset_id });
			index_id = op->id;
			impl.add(op);
		}
	}

	return { index_id, num_components };
}

bool emit_buffer_load_instruction(Converter::Impl &impl, const llvm::CallInst *instruction)
{
	auto &builder = impl.builder();
	spv::Id image_id = impl.get_id_for_value(instruction->getOperand(1));
	spv::Id image_type_id = impl.get_type_id(image_id);
	bool is_uav = builder.isStorageImageType(image_type_id);
	const auto &meta = impl.handle_to_resource_meta[image_id];
	bool is_typed = meta.kind == DXIL::ResourceKind::TypedBuffer;

	auto access = build_buffer_access(impl, instruction);
	auto *result_type = instruction->getType();

	if (!is_typed)
	{
		// Unroll 4 loads. Ideally, we'd probably use physical_storage_buffer here, but unfortunately we have no indication
		// how many components we need to load here, and the number of components we load is not necessarily constant,
		// so we cannot reliably encode this information in the SRV.
		// The best we can do is to infer it from stride if we can.

		// For raw buffers, we have no stride information, so assume we need to load 4 components.
		// Hopefully compiler can eliminate loads which are never used ...
		unsigned conservative_num_elements = access.num_components;

		spv::Id component_ids[4] = {};

		spv::Id extracted_id_type = builder.makeUintType(32);
		spv::Id loaded_id_type = builder.makeVectorType(extracted_id_type, 4);

		for (unsigned i = 0; i < conservative_num_elements; i++)
		{
			Operation *loaded_op = impl.allocate(is_uav ? spv::OpImageRead : spv::OpImageFetch, loaded_id_type);
			loaded_op->add_ids({ image_id, impl.build_offset(access.index_id, i) });
			impl.add(loaded_op);

			Operation *extracted_op = impl.allocate(spv::OpCompositeExtract, extracted_id_type);
			extracted_op->add_id(loaded_op->id);
			extracted_op->add_literal(0);
			impl.add(extracted_op);

			component_ids[i] = extracted_op->id;
		}

		// We expect that we will extract components from the resulting struct, so make a fake composite if we need to.
		if (conservative_num_elements < 2)
		{
			for (unsigned i = conservative_num_elements; i < 2; i++)
				component_ids[i] = builder.createUndefined(builder.makeUintType(32));
			conservative_num_elements = 2;
		}

		bool need_bitcast = result_type->getStructElementType(0)->getTypeID() != llvm::Type::TypeID::IntegerTyID;

		spv::Id constructed_id = impl.build_vector(extracted_id_type, component_ids, conservative_num_elements);

		if (need_bitcast)
		{
			Operation *op = impl.allocate(spv::OpBitcast,
			                              impl.build_vector_type(impl.get_type_id(result_type->getStructElementType(0)),
			                                                     conservative_num_elements));

			op->add_id(constructed_id);
			impl.add(op);
			impl.value_map[instruction] = op->id;
		}
		else
			impl.value_map[instruction] = constructed_id;
	}
	else
	{
		Operation *op = impl.allocate(is_uav ? spv::OpImageRead : spv::OpImageFetch, instruction,
		                              impl.get_type_id(meta.component_type, 1, 4));

		op->add_ids({ image_id, access.index_id });
		impl.add(op);

		// Deal with loads from signed resources.
		impl.fixup_load_sign(meta.component_type, 4, instruction);
	}

	// TODO: Might have an option to rely on StorageImageReadWithoutFormat.
	//if (is_uav && is_typed)
	//	builder.addCapability(spv::CapabilityStorageImageReadWithoutFormat);

	return true;
}

static spv::Id build_physical_pointer_address_for_raw_load_store(Converter::Impl &impl, const llvm::CallInst *instruction)
{
	auto &builder = impl.builder();
	spv::Id ptr_id = impl.get_id_for_value(instruction->getOperand(1));
	const auto &meta = impl.handle_to_resource_meta[ptr_id];

	spv::Id index_id = impl.get_id_for_value(instruction->getOperand(2));
	spv::Id element_offset = 0;
	if (meta.stride != 0)
		element_offset = impl.get_id_for_value(instruction->getOperand(3));

	Operation *u64_offset_op;
	if (meta.stride)
	{
		auto *stride_op = impl.allocate(spv::OpIMul, builder.makeUintType(32));
		stride_op->add_id(index_id);
		stride_op->add_id(builder.makeUintConstant(meta.stride));
		impl.add(stride_op);

		auto *offset_op = impl.allocate(spv::OpIAdd, builder.makeUintType(32));
		offset_op->add_id(stride_op->id);
		offset_op->add_id(element_offset);
		impl.add(offset_op);

		u64_offset_op = impl.allocate(spv::OpUConvert, builder.makeUintType(64));
		u64_offset_op->add_id(offset_op->id);
	}
	else
	{
		u64_offset_op = impl.allocate(spv::OpUConvert, builder.makeUintType(64));
		u64_offset_op->add_id(index_id);
	}

	impl.add(u64_offset_op);

	auto *ptr_compute_op = impl.allocate(spv::OpIAdd, builder.makeUintType(64));
	ptr_compute_op->add_id(ptr_id);
	ptr_compute_op->add_id(u64_offset_op->id);
	impl.add(ptr_compute_op);

	return ptr_compute_op->id;
}

bool emit_raw_buffer_load_instruction(Converter::Impl &impl, const llvm::CallInst *instruction)
{
	auto &builder = impl.builder();
	spv::Id ptr_id = impl.get_id_for_value(instruction->getOperand(1));
	const auto &meta = impl.handle_to_resource_meta[ptr_id];

	uint32_t mask = 0;
	if (!get_constant_operand(instruction, 4, &mask))
		return false;

	uint32_t alignment = 0;
	if (!get_constant_operand(instruction, 5, &alignment))
		return false;

	if (meta.storage != spv::StorageClassPhysicalStorageBuffer)
	{
		// If we're attempting a raw load from a non-physical pointer, it gets spicy.
		// Since we're using texel buffers for this case, we might not be able to implement it correctly.
		if (alignment < 4)
		{
			LOGE("Requested an alignment of < 4 bytes in RawBufferLoad with descriptor. This is unimplementable.\n");
			return false;
		}

		auto *ret_component = instruction->getType()->getStructElementType(0);
		if (ret_component->getTypeID() != llvm::Type::TypeID::FloatTyID &&
			!(ret_component->getTypeID() == llvm::Type::TypeID::IntegerTyID && ret_component->getIntegerBitWidth() == 32))
		{
			LOGE("RawBufferLoad on descriptors is only supported for 32-bits currently.\n");
			return false;
		}

		// Ignore the mask. We'll read too much, but robustness should take care of any OOB.
		return emit_buffer_load_instruction(impl, instruction);
	}

	unsigned vecsize = 0;
	if (mask == 1)
		vecsize = 1;
	else if (mask == 3)
		vecsize = 2;
	else if (mask == 7)
		vecsize = 3;
	else if (mask == 15)
		vecsize = 4;
	else
	{
		LOGE("Unexpected mask for RawBufferLoad = %u.\n", mask);
		return false;
	}

	spv::Id type_id = impl.get_type_id(instruction->getType()->getStructElementType(0));
	if (vecsize > 1)
		type_id = builder.makeVectorType(type_id, vecsize);
	spv::Id ptr_type_id = builder.makePointer(spv::StorageClassPhysicalStorageBuffer, type_id);

	spv::Id u64_ptr_id = build_physical_pointer_address_for_raw_load_store(impl, instruction);

	auto *ptr_bitcast_op = impl.allocate(spv::OpBitcast, ptr_type_id);
	ptr_bitcast_op->add_id(u64_ptr_id);
	impl.add(ptr_bitcast_op);

	auto *load_op = impl.allocate(spv::OpLoad, instruction, type_id);
	load_op->add_id(ptr_bitcast_op->id);
	load_op->add_literal(spv::MemoryAccessAlignedMask);
	load_op->add_literal(alignment);
	impl.add(load_op);

	if (vecsize == 1)
	{
		// Need to present a composite to caller, swizzle in an OpUndef.
		spv::Id vec_type_id = builder.makeVectorType(type_id, 2);
		auto *composite_op = impl.allocate(spv::OpCompositeConstruct, vec_type_id);
		composite_op->add_id(load_op->id);
		composite_op->add_id(builder.createUndefined(type_id));
		impl.add(composite_op);
		impl.value_map[instruction] = composite_op->id;
	}

	return true;
}

bool emit_buffer_store_instruction(Converter::Impl &impl, const llvm::CallInst *instruction)
{
	auto &builder = impl.builder();
	spv::Id image_id = impl.get_id_for_value(instruction->getOperand(1));
	const auto &meta = impl.handle_to_resource_meta[image_id];
	auto access = build_buffer_access(impl, instruction);

	spv::Id store_values[4] = {};
	unsigned mask = llvm::cast<llvm::ConstantInt>(instruction->getOperand(8))->getUniqueInteger().getZExtValue();
	bool is_typed = meta.kind == DXIL::ResourceKind::TypedBuffer;

	for (unsigned i = 0; i < 4; i++)
	{
		store_values[i] = impl.get_id_for_value(instruction->getOperand(4 + i));
		if (!is_typed && (mask & (1u << i)))
		{
			if (instruction->getOperand(4 + i)->getType()->getTypeID() != llvm::Type::TypeID::IntegerTyID)
			{
				Operation *op = impl.allocate(spv::OpBitcast, builder.makeUintType(32));
				op->add_id(store_values[i]);
				store_values[i] = op->id;
				impl.add(op);
			}
		}
	}

	if (is_typed)
	{
		spv::Id element_type_id = impl.get_type_id(instruction->getOperand(4)->getType());

		// Deal with signed resource store.
		Operation *op = impl.allocate(spv::OpImageWrite);
		op->add_ids(
		    { image_id, access.index_id,
		      impl.fixup_store_sign(meta.component_type, 4, impl.build_vector(element_type_id, store_values, 4)) });

		impl.add(op);
	}
	else
	{
		spv::Id splat_type_id = builder.makeVectorType(builder.makeUintType(32), 4);
		for (unsigned i = 0; i < 4; i++)
		{
			if (mask & (1u << i))
			{
				Operation *splat_op = impl.allocate(spv::OpCompositeConstruct, splat_type_id);
				splat_op->add_ids({ store_values[i], store_values[i], store_values[i], store_values[i] });
				impl.add(splat_op);

				Operation *op = impl.allocate(spv::OpImageWrite);
				op->add_ids({
				    image_id,
				    impl.build_offset(access.index_id, i),
				    splat_op->id,
				});
				impl.add(op);
			}
		}
	}

	if (is_typed)
		builder.addCapability(spv::CapabilityStorageImageWriteWithoutFormat);

	return true;
}

bool emit_raw_buffer_store_instruction(Converter::Impl &impl, const llvm::CallInst *instruction)
{
	auto &builder = impl.builder();
	spv::Id ptr_id = impl.get_id_for_value(instruction->getOperand(1));
	const auto &meta = impl.handle_to_resource_meta[ptr_id];

	uint32_t mask = 0;
	if (!get_constant_operand(instruction, 8, &mask))
		return false;

	uint32_t alignment = 0;
	if (!get_constant_operand(instruction, 9, &alignment))
		return false;

	if (meta.storage != spv::StorageClassPhysicalStorageBuffer)
	{
		// If we're attempting a raw load from a non-physical pointer, it gets spicy.
		// Since we're using texel buffers for this case, we might not be able to implement it correctly.
		if (alignment < 4)
		{
			LOGE("Requested an alignment of < 4 bytes in RawBufferLoad with descriptor. This is unimplementable.\n");
			return false;
		}

		auto *store_type = instruction->getOperand(4)->getType();
		if (store_type->getTypeID() != llvm::Type::TypeID::FloatTyID &&
		    !(store_type->getTypeID() == llvm::Type::TypeID::IntegerTyID && store_type->getIntegerBitWidth() == 32))
		{
			LOGE("RawBufferStore on descriptors is only supported for 32-bits currently.\n");
			return false;
		}

		return emit_buffer_store_instruction(impl, instruction);
	}

	unsigned vecsize = 0;
	if (mask == 1)
		vecsize = 1;
	else if (mask == 3)
		vecsize = 2;
	else if (mask == 7)
		vecsize = 3;
	else if (mask == 15)
		vecsize = 4;
	else
	{
		LOGE("Unexpected mask for RawBufferStore = %u.\n", mask);
		return false;
	}

	spv::Id type_id = impl.get_type_id(instruction->getOperand(4)->getType());
	spv::Id vec_type_id = type_id;
	if (vecsize > 1)
		vec_type_id = builder.makeVectorType(type_id, vecsize);
	spv::Id ptr_type_id = builder.makePointer(spv::StorageClassPhysicalStorageBuffer, vec_type_id);

	spv::Id u64_ptr_id = build_physical_pointer_address_for_raw_load_store(impl, instruction);

	auto *ptr_bitcast_op = impl.allocate(spv::OpBitcast, ptr_type_id);
	ptr_bitcast_op->add_id(u64_ptr_id);
	impl.add(ptr_bitcast_op);

	spv::Id elems[4] = {};
	for (unsigned i = 0; i < 4; i++)
		elems[i] = impl.get_id_for_value(instruction->getOperand(4 + i));

	auto *store_op = impl.allocate(spv::OpStore);
	store_op->add_id(ptr_bitcast_op->id);
	store_op->add_id(impl.build_vector(type_id, elems, vecsize));
	store_op->add_literal(spv::MemoryAccessAlignedMask);
	store_op->add_literal(alignment);
	impl.add(store_op);

	return true;
}

bool emit_atomic_binop_instruction(Converter::Impl &impl, const llvm::CallInst *instruction)
{
	auto &builder = impl.builder();
	spv::Id image_id = impl.get_id_for_value(instruction->getOperand(1));
	const auto &meta = impl.handle_to_resource_meta[image_id];
	auto binop = static_cast<DXIL::AtomicBinOp>(
	    llvm::cast<llvm::ConstantInt>(instruction->getOperand(2))->getUniqueInteger().getZExtValue());

	spv::Id coords[3] = {};

	uint32_t num_coords_full = 0, num_coords = 0;
	if (!get_image_dimensions(impl, image_id, &num_coords_full, &num_coords))
		return false;

	if (num_coords_full > 3)
		return false;

	if (meta.kind == DXIL::ResourceKind::StructuredBuffer || meta.kind == DXIL::ResourceKind::RawBuffer)
	{
		auto access = build_buffer_access(impl, instruction, 1);
		coords[0] = access.index_id;
	}
	else
	{
		for (uint32_t i = 0; i < num_coords_full; i++)
			coords[i] = impl.get_id_for_value(instruction->getOperand(3 + i));
	}
	spv::Id coord = impl.build_vector(builder.makeUintType(32), coords, num_coords_full);

	Operation *counter_ptr_op =
	    impl.allocate(spv::OpImageTexelPointer,
	                  builder.makePointer(spv::StorageClassImage, impl.get_type_id(meta.component_type, 1, 1)));
	counter_ptr_op->add_ids({ meta.var_id, coord, builder.makeUintConstant(0) });
	impl.add(counter_ptr_op);

	if (meta.non_uniform)
		builder.addDecoration(counter_ptr_op->id, spv::DecorationNonUniformEXT);

	spv::Op opcode;

	switch (binop)
	{
	case DXIL::AtomicBinOp::Exchange:
		opcode = spv::OpAtomicExchange;
		break;

	case DXIL::AtomicBinOp::IAdd:
		opcode = spv::OpAtomicIAdd;
		break;

	case DXIL::AtomicBinOp::And:
		opcode = spv::OpAtomicAnd;
		break;

	case DXIL::AtomicBinOp::Or:
		opcode = spv::OpAtomicOr;
		break;

	case DXIL::AtomicBinOp::Xor:
		opcode = spv::OpAtomicXor;
		break;

	case DXIL::AtomicBinOp::IMin:
		opcode = spv::OpAtomicSMin;
		break;

	case DXIL::AtomicBinOp::IMax:
		opcode = spv::OpAtomicSMax;
		break;

	case DXIL::AtomicBinOp::UMin:
		opcode = spv::OpAtomicUMin;
		break;

	case DXIL::AtomicBinOp::UMax:
		opcode = spv::OpAtomicUMax;
		break;

	default:
		return false;
	}

	Operation *op = impl.allocate(opcode, instruction, impl.get_type_id(meta.component_type, 1, 1));
	op->add_ids({
	    counter_ptr_op->id,
	    builder.makeUintConstant(spv::ScopeDevice),
	    builder.makeUintConstant(0), // Relaxed
	    impl.fixup_store_sign(meta.component_type, 1, impl.get_id_for_value(instruction->getOperand(6))),
	});

	impl.add(op);
	impl.fixup_load_sign(meta.component_type, 1, instruction);
	return true;
}

bool emit_atomic_cmpxchg_instruction(Converter::Impl &impl, const llvm::CallInst *instruction)
{
	auto &builder = impl.builder();
	spv::Id image_id = impl.get_id_for_value(instruction->getOperand(1));
	const auto &meta = impl.handle_to_resource_meta[image_id];
	spv::Id coords[3] = {};

	uint32_t num_coords_full = 0, num_coords = 0;
	if (!get_image_dimensions(impl, image_id, &num_coords_full, &num_coords))
		return false;

	if (num_coords_full > 3)
		return false;

	if (meta.kind == DXIL::ResourceKind::StructuredBuffer || meta.kind == DXIL::ResourceKind::RawBuffer)
	{
		auto access = build_buffer_access(impl, instruction);
		coords[0] = access.index_id;
	}
	else
	{
		for (uint32_t i = 0; i < num_coords_full; i++)
			coords[i] = impl.get_id_for_value(instruction->getOperand(2 + i));
	}

	spv::Id coord = impl.build_vector(builder.makeUintType(32), coords, num_coords_full);

	Operation *counter_ptr_op =
	    impl.allocate(spv::OpImageTexelPointer,
	                  builder.makePointer(spv::StorageClassImage, impl.get_type_id(meta.component_type, 1, 1)));
	counter_ptr_op->add_ids({ meta.var_id, coord, builder.makeUintConstant(0) });
	impl.add(counter_ptr_op);

	Operation *op =
	    impl.allocate(spv::OpAtomicCompareExchange, instruction, impl.get_type_id(meta.component_type, 1, 1));

	spv::Id comparison_id = impl.get_id_for_value(instruction->getOperand(5));
	spv::Id new_value_id = impl.get_id_for_value(instruction->getOperand(6));
	comparison_id = impl.fixup_store_sign(meta.component_type, 1, comparison_id);
	new_value_id = impl.fixup_store_sign(meta.component_type, 1, new_value_id);

	op->add_ids({
	    counter_ptr_op->id,
	    builder.makeUintConstant(spv::ScopeDevice),
	    builder.makeUintConstant(0), // Relaxed
	    builder.makeUintConstant(0), // Relaxed
	    new_value_id,
	    comparison_id,
	});

	impl.add(op);
	impl.fixup_load_sign(meta.component_type, 1, instruction);
	return true;
}

bool emit_buffer_update_counter_instruction(Converter::Impl &impl, const llvm::CallInst *instruction)
{
	auto &builder = impl.builder();
	spv::Id image_id = impl.get_id_for_value(instruction->getOperand(1));
	const auto &meta = impl.handle_to_resource_meta[image_id];
	int direction = llvm::cast<llvm::ConstantInt>(instruction->getOperand(2))->getUniqueInteger().getSExtValue();

	Operation *counter_ptr_op;

	if (meta.counter_is_physical_pointer)
	{
		counter_ptr_op = impl.allocate(
		    spv::OpAccessChain, builder.makePointer(spv::StorageClassPhysicalStorageBuffer, builder.makeUintType(32)));
		counter_ptr_op->add_id(meta.counter_var_id);
		counter_ptr_op->add_id(builder.makeUintConstant(0));
	}
	else
	{
		counter_ptr_op = impl.allocate(spv::OpImageTexelPointer,
		                               builder.makePointer(spv::StorageClassImage, builder.makeUintType(32)));
		counter_ptr_op->add_ids({ meta.counter_var_id, builder.makeUintConstant(0), builder.makeUintConstant(0) });
	}

	impl.add(counter_ptr_op);

	Operation *op = impl.allocate(spv::OpAtomicIAdd, instruction);
	op->add_ids({ counter_ptr_op->id, builder.makeUintConstant(spv::ScopeDevice),
	              builder.makeUintConstant(0), // Relaxed.
	              builder.makeUintConstant(direction) });

	impl.add(op);

	if (direction < 0)
	{
		spv::Id result_id = op->id;
		op = impl.allocate(spv::OpISub, builder.makeUintType(32));
		op->add_ids({ result_id, builder.makeUintConstant(1) });
		impl.add(op);
		impl.value_map[instruction] = op->id;
	}

	return true;
}

} // namespace dxil_spv
