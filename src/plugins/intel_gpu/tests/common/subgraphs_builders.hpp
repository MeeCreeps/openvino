// Copyright (C) 2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <memory>
#include "openvino/core/dimension.hpp"
#include "openvino/core/model.hpp"
#include "openvino/core/node_vector.hpp"
#include "openvino/core/partial_shape.hpp"
#include "openvino/op/broadcast.hpp"
#include "openvino/op/constant.hpp"
#include "openvino/op/gather.hpp"
#include "openvino/op/read_value.hpp"
#include "openvino/op/reshape.hpp"
#include "openvino/op/shape_of.hpp"
#include "openvino/op/transpose.hpp"
#include "openvino/op/result.hpp"
#include "openvino/op/parameter.hpp"
#include "openvino/op/matmul.hpp"
#include "openvino/op/convert.hpp"
#include "openvino/op/concat.hpp"
#include "openvino/op/unsqueeze.hpp"
#include "openvino/op/util/read_value_base.hpp"
#include "openvino/pass/make_stateful.hpp"
#include "openvino/pass/visualize_tree.hpp"

namespace tests {

inline std::shared_ptr<ov::Model> make_llm_kv_cache_pattern(ov::Dimension batch = ov::Dimension::dynamic(),
                                                            ov::Dimension n_heads = ov::Dimension::dynamic(),
                                                            ov::Dimension n_features = ov::Dimension::dynamic(),
                                                            ov::element::Type_t element_type = ov::element::f32,
                                                            int64_t concat_axis = 2,
                                                            bool stateful = false,
                                                            bool fuse_cache_reorder = false,
                                                            bool build_state_initializer = false,
                                                            size_t num_groups = 1) {
    ov::PartialShape kv_cache_size = {batch, n_heads / num_groups, -1, n_features};
    ov::PartialShape new_token_size = {batch, -1, n_heads / num_groups, n_features};
    ov::PartialShape matmul_in_size = {batch, n_heads, -1, -1};

    auto in_kv_prev = std::make_shared<ov::op::v0::Parameter>(element_type, kv_cache_size);
    in_kv_prev->set_friendly_name("past_key_values");
    auto in_new_token = std::make_shared<ov::op::v0::Parameter>(element_type, new_token_size);
    in_new_token->set_friendly_name("new_token_input");
    auto in_matmul = std::make_shared<ov::op::v0::Parameter>(element_type, matmul_in_size);
    in_matmul->set_friendly_name("in_matmul");

    ov::ParameterVector params{in_kv_prev, in_new_token, in_matmul};
    std::shared_ptr<ov::Node> concat_input = in_kv_prev;
    if (fuse_cache_reorder) {
        auto in_beam_idx = std::make_shared<ov::op::v0::Parameter>(ov::element::i32, ov::PartialShape{batch});
        in_beam_idx->set_friendly_name("beam_idx");
        params.push_back(in_beam_idx);
        auto axis = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
        auto gather = std::make_shared<ov::op::v8::Gather>(in_kv_prev, in_beam_idx, axis, 0);
        concat_input = gather;
    }

    std::shared_ptr<ov::Node> state_initializer = nullptr;
    if (stateful && build_state_initializer) {
        auto shapeof = std::make_shared<ov::op::v3::ShapeOf>(in_new_token, ov::element::i32);

        auto indices = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{1}, 0);
        auto axis = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
        auto gather = std::make_shared<ov::op::v8::Gather>(shapeof, indices, axis, 0);

        auto bcast_value = std::make_shared<ov::op::v0::Constant>(element_type, ov::Shape{}, 0.0f);
        ov::NodeVector dims = {gather};
        for (size_t i = 1; i < kv_cache_size.size(); i++) {
            dims.push_back(std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{1}, static_cast<int32_t>(kv_cache_size[i].get_min_length())));
        }
        auto shape = std::make_shared<ov::op::v0::Concat>(dims, 0);
        state_initializer = std::make_shared<ov::op::v3::Broadcast>(bcast_value, shape);
    }

    auto transpose_const = ov::op::v0::Constant::create(ov::element::i32, {new_token_size.size()}, {0, 2, 1, 3});
    auto transpose = std::make_shared<ov::op::v1::Transpose>(in_new_token, transpose_const);
    auto concat = std::make_shared<ov::op::v0::Concat>(ov::OutputVector{concat_input, transpose}, concat_axis);
    auto convert = std::make_shared<ov::op::v0::Convert>(concat, element_type);
    auto kv_present = std::make_shared<ov::op::v0::Result>(convert);
    kv_present->set_friendly_name("present_key_values");

    std::shared_ptr<ov::Node> kv_input = concat;
    if (num_groups > 1) {
        auto unsqueeze_axis = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{1}, static_cast<int32_t>(2));
        auto unsqueeze = std::make_shared<ov::op::v0::Unsqueeze>(concat, unsqueeze_axis);


        // prepare broadcast shape
        auto concat_shape_of = std::make_shared<ov::op::v3::ShapeOf>(concat, ov::element::i32);

        auto indices01 = ov::op::v0::Constant::create(ov::element::i32, {2}, {0, 1});
        auto axis01 = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
        auto gather01 = std::make_shared<ov::op::v8::Gather>(concat_shape_of, indices01, axis01, 0);

        auto indices23 = ov::op::v0::Constant::create(ov::element::i32, {2}, {2, 3});
        auto axis23 = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{}, 0);
        auto gather23 = std::make_shared<ov::op::v8::Gather>(concat_shape_of, indices23, axis23, 0);

        auto groups = std::make_shared<ov::op::v0::Constant>(ov::element::i32, ov::Shape{1}, static_cast<int32_t>(num_groups));


        auto shape_concat = std::make_shared<ov::op::v0::Concat>(ov::NodeVector{gather01, groups, gather23}, 0);
        auto broadcast = std::make_shared<ov::op::v3::Broadcast>(unsqueeze, shape_concat);

        std::vector<int32_t> s = { 0, static_cast<int32_t>(n_heads.get_length()), -1, static_cast<int32_t>(n_features.get_length()) };
        auto target_shape = ov::op::v0::Constant::create(ov::element::i32, {4}, s);
        auto reshape = std::make_shared<ov::op::v1::Reshape>(broadcast, target_shape, true);

        kv_input = reshape;
    }

    auto matmul = std::make_shared<ov::op::v0::MatMul>(in_matmul, kv_input, false, false);
    auto matmul_out = std::make_shared<ov::op::v0::Result>(matmul);
    matmul_out->set_friendly_name("matmul_out");

    ov::ResultVector results{kv_present, matmul_out};
    auto model = std::make_shared<ov::Model>(results, params, "LLM-KV-Cache");
    ov::pass::VisualizeTree("model.svg").run_on_model(model);
    if (stateful) {
        ov::pass::MakeStateful({{in_kv_prev, kv_present}}).run_on_model(model);
    }

    if (state_initializer) {
        for (auto op : model->get_ops()) {
            if (auto read_value = std::dynamic_pointer_cast<ov::op::v6::ReadValue>(op)) {
                read_value->set_arguments(ov::OutputVector{state_initializer});
                break;
            }
        }
    }
    model->validate_nodes_and_infer_types();

    return model;
}

} // namespace tests
