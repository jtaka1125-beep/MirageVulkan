#!/usr/bin/env python3
"""
TFLite placeholder モデル生成スクリプト
TensorFlowなしで、FlatBufferバイナリを直接構築する

生成するモデル:
1. screen_classifier.tflite - [1,224,224,3] -> [1,8] (Conv2D+GlobalAvgPool)
2. ui_detector.tflite - [1,320,320,3] -> SSD-like 4出力
"""

import struct
import os
import sys
import numpy as np

# TFLite FlatBuffer を手動構築するためのヘルパー
# TFLite schema: https://github.com/tensorflow/tensorflow/blob/master/tensorflow/lite/schema/schema.fbs

class FlatBufferBuilder:
    """最小限のFlatBufferビルダー"""
    def __init__(self, initial_size=1024):
        self.buf = bytearray(initial_size)
        self.head = initial_size  # 末尾から書き込む
        self.vtables = []
        self.nested = False
        self.finished = False
        self.minalign = 1
        self.current_vtable = None
        self.object_start = 0
        self.num_fields = 0

    def _grow(self, needed):
        while self.head < needed:
            old = self.buf
            self.buf = bytearray(len(old) * 2)
            self.buf[len(self.buf) - len(old):] = old
            self.head += len(self.buf) // 2

    def offset(self):
        return len(self.buf) - self.head

    def pad(self, n):
        self._grow(n)
        self.head -= n
        for i in range(n):
            self.buf[self.head + i] = 0

    def align(self, size, additional_bytes=0):
        align_size = (~(self.offset() + additional_bytes) + 1) & (size - 1)
        self.pad(align_size)

    def place_byte(self, x):
        self._grow(1)
        self.head -= 1
        struct.pack_into('<B', self.buf, self.head, x)

    def place_int16(self, x):
        self._grow(2)
        self.head -= 2
        struct.pack_into('<h', self.buf, self.head, x)

    def place_uint16(self, x):
        self._grow(2)
        self.head -= 2
        struct.pack_into('<H', self.buf, self.head, x)

    def place_int32(self, x):
        self._grow(4)
        self.head -= 4
        struct.pack_into('<i', self.buf, self.head, x)

    def place_uint32(self, x):
        self._grow(4)
        self.head -= 4
        struct.pack_into('<I', self.buf, self.head, x)

    def place_float32(self, x):
        self._grow(4)
        self.head -= 4
        struct.pack_into('<f', self.buf, self.head, x)

    def create_string(self, s):
        if isinstance(s, str):
            s = s.encode('utf-8')
        self.align(4, len(s) + 1)
        self.place_byte(0)  # null terminator
        self._grow(len(s))
        self.head -= len(s)
        self.buf[self.head:self.head + len(s)] = s
        self.place_int32(len(s))
        return self.offset()

    def create_vector(self, elem_size, data_func, count):
        self.align(4, count * elem_size)
        # 要素を逆順に配置
        data_func()
        self.place_int32(count)
        return self.offset()

    def create_byte_vector(self, data):
        self.align(4, len(data))
        self._grow(len(data))
        self.head -= len(data)
        self.buf[self.head:self.head + len(data)] = data
        self.place_int32(len(data))
        return self.offset()

    def create_int32_vector(self, values):
        self.align(4, len(values) * 4)
        for v in reversed(values):
            self.place_int32(v)
        self.place_int32(len(values))
        return self.offset()

    def create_uint8_vector(self, values):
        self.align(4, len(values))
        for v in reversed(values):
            self.place_byte(v)
        self.place_int32(len(values))
        return self.offset()

    def create_offset_vector(self, offsets):
        self.align(4, len(offsets) * 4)
        for off in reversed(offsets):
            self.place_uint32(off)  # soffset
        self.place_int32(len(offsets))
        return self.offset()

    def start_object(self, num_fields):
        self.current_vtable = [0] * num_fields
        self.num_fields = num_fields
        self.object_start = self.offset()

    def add_field_int8(self, field_idx, value, default=0):
        if value != default:
            self.align(1)
            self.place_byte(value)
            self.current_vtable[field_idx] = self.offset()

    def add_field_int32(self, field_idx, value, default=0):
        if value != default:
            self.align(4)
            self.place_int32(value)
            self.current_vtable[field_idx] = self.offset()

    def add_field_offset(self, field_idx, offset):
        if offset != 0:
            self.align(4)
            self.place_uint32(self.offset() - offset + 4)
            self.current_vtable[field_idx] = self.offset()

    def add_field_union_type(self, field_idx, value):
        self.add_field_int8(field_idx, value)

    def end_object(self):
        self.align(4)
        self.place_int32(0)  # placeholder for vtable offset
        obj_end = self.offset()

        # vtableを構築
        vtable_size = 4 + len(self.current_vtable) * 2
        obj_size = obj_end - self.object_start

        # 既存vtableとの重複チェックは省略（簡略化）
        # vtableを書き込む
        self.align(2)
        for i in reversed(range(len(self.current_vtable))):
            off = self.current_vtable[i]
            if off != 0:
                self.place_uint16(obj_end - off)
            else:
                self.place_uint16(0)
        self.place_uint16(obj_size)
        self.place_uint16(vtable_size)

        vtable_offset = self.offset()

        # オブジェクトの先頭のvtableポインタを修正
        buf_pos = len(self.buf) - obj_end
        struct.pack_into('<i', self.buf, buf_pos, obj_end - vtable_offset)

        return obj_end

    def finish(self, root_table_offset):
        self.align(4, 4)  # file identifier用スペースなし
        self.place_uint32(self.offset() - root_table_offset + 4)
        self.finished = True

    def finish_with_file_id(self, root_table_offset, file_id):
        self.align(4, 8)
        if isinstance(file_id, str):
            file_id = file_id.encode('utf-8')
        for i in range(3, -1, -1):
            self.place_byte(file_id[i] if i < len(file_id) else 0)
        self.place_uint32(self.offset() - root_table_offset + 4)
        self.finished = True

    def output(self):
        return bytes(self.buf[self.head:])


def build_tflite_model_raw(tensors, operators, subgraph_inputs, subgraph_outputs,
                           op_codes, buffers_data, description="placeholder"):
    """
    TFLiteモデルをバイナリで直接構築する。

    より信頼性の高いアプローチ: struct.packで直接FlatBufferを構築
    """
    # FlatBufferの手動構築は複雑すぎるので、
    # 最小限の有効なTFLiteファイルを生成する簡略アプローチを使用

    b = FlatBufferBuilder(4096)

    # バッファを作成
    buffer_offsets = []
    for buf_data in buffers_data:
        if buf_data is not None and len(buf_data) > 0:
            data_vec = b.create_byte_vector(buf_data)
        else:
            data_vec = 0

        b.start_object(2)  # Buffer has 2 fields: data, offset (deprecated)
        if data_vec:
            b.add_field_offset(0, data_vec)  # data
        buffer_offsets.append(b.end_object())

    buffers_vec = b.create_offset_vector(buffer_offsets)

    # テンソルを作成
    tensor_offsets = []
    for t in tensors:
        name_off = b.create_string(t['name'])
        shape_vec = b.create_int32_vector(t['shape'])

        # Tensor table: shape(0), type(1), buffer(2), name(3), quantization(4)
        b.start_object(5)
        b.add_field_offset(0, shape_vec)  # shape
        b.add_field_int8(1, t['type'])    # type (0=FLOAT32)
        b.add_field_int32(2, t['buffer']) # buffer index
        b.add_field_offset(3, name_off)   # name
        tensor_offsets.append(b.end_object())

    tensors_vec = b.create_offset_vector(tensor_offsets)

    # オペレータを作成
    op_offsets = []
    for op in operators:
        inputs_vec = b.create_int32_vector(op['inputs'])
        outputs_vec = b.create_int32_vector(op['outputs'])

        # Operator: opcode_index(0), inputs(1), outputs(2), builtin_options_type(3),
        #           builtin_options(4), custom_options(5), custom_options_format(6)
        b.start_object(7)
        b.add_field_int32(0, op['opcode_index'])
        b.add_field_offset(1, inputs_vec)
        b.add_field_offset(2, outputs_vec)
        op_offsets.append(b.end_object())

    operators_vec = b.create_offset_vector(op_offsets)

    # サブグラフ入力・出力
    inputs_vec = b.create_int32_vector(subgraph_inputs)
    outputs_vec = b.create_int32_vector(subgraph_outputs)
    sg_name = b.create_string("main")

    # SubGraph: tensors(0), inputs(1), outputs(2), operators(3), name(4)
    b.start_object(5)
    b.add_field_offset(0, tensors_vec)
    b.add_field_offset(1, inputs_vec)
    b.add_field_offset(2, outputs_vec)
    b.add_field_offset(3, operators_vec)
    b.add_field_offset(4, sg_name)
    subgraph_off = b.end_object()

    subgraphs_vec = b.create_offset_vector([subgraph_off])

    # OperatorCode を作成
    opcode_offsets = []
    for oc in op_codes:
        # OperatorCode: deprecated_builtin_code(0), custom_code(1), version(2), builtin_code(3)
        b.start_object(4)
        # deprecated field (int8, capped at 127)
        dep_code = min(oc, 127)
        b.add_field_int8(0, dep_code)
        b.add_field_int32(2, 1)  # version
        b.add_field_int32(3, oc)  # builtin_code (int32)
        opcode_offsets.append(b.end_object())

    opcodes_vec = b.create_offset_vector(opcode_offsets)

    # description
    desc_off = b.create_string(description)

    # Model: version(0), operator_codes(1), subgraphs(2), description(3), buffers(4)
    b.start_object(5)
    b.add_field_int32(0, 3)  # schema version 3
    b.add_field_offset(1, opcodes_vec)
    b.add_field_offset(2, subgraphs_vec)
    b.add_field_offset(3, desc_off)
    b.add_field_offset(4, buffers_vec)
    model_off = b.end_object()

    b.finish_with_file_id(model_off, "TFL3")

    return b.output()


def generate_screen_classifier(output_path):
    """
    screen_classifier.tflite を生成
    入力: [1, 224, 224, 3] float32
    出力: [1, 8] float32
    構造: Conv2D(3x3, 8filters) -> MEAN (GlobalAvgPool) -> 出力
    """
    # Conv2D weights: [8, 3, 3, 3] = 216 floats
    conv_weights = np.zeros((8, 3, 3, 3), dtype=np.float32)
    # 各フィルタに小さな値を設定
    for i in range(8):
        conv_weights[i, 1, 1, i % 3] = 0.1  # center pixel
    conv_weight_data = conv_weights.tobytes()

    # Conv2D bias: [8]
    conv_bias = np.zeros(8, dtype=np.float32)
    conv_bias_data = conv_bias.tobytes()

    # MEAN の reduction_indices: [1, 2] (spatial dims)
    mean_axes = np.array([1, 2], dtype=np.int32)
    mean_axes_data = mean_axes.tobytes()

    # バッファ: 0=empty(入力), 1=conv_weights, 2=conv_bias, 3=mean_axes, 4=empty(conv_out), 5=empty(output)
    buffers = [
        None,               # 0: 空（入力テンソル用）
        conv_weight_data,   # 1: Conv2D weights
        conv_bias_data,     # 2: Conv2D bias
        mean_axes_data,     # 3: MEAN axes
        None,               # 4: 空（Conv2D出力用）
        None,               # 5: 空（最終出力用）
    ]

    tensors = [
        {'name': 'input', 'shape': [1, 224, 224, 3], 'type': 0, 'buffer': 0},       # 0
        {'name': 'conv_weights', 'shape': [8, 3, 3, 3], 'type': 0, 'buffer': 1},    # 1
        {'name': 'conv_bias', 'shape': [8], 'type': 0, 'buffer': 2},                 # 2
        {'name': 'conv_output', 'shape': [1, 222, 222, 8], 'type': 0, 'buffer': 4}, # 3
        {'name': 'mean_axes', 'shape': [2], 'type': 2, 'buffer': 3},                # 4 (type 2 = INT32)
        {'name': 'output', 'shape': [1, 8], 'type': 0, 'buffer': 5},                # 5
    ]

    # Conv2D = opcode 3, MEAN = opcode 25
    operators = [
        {'opcode_index': 0, 'inputs': [0, 1, 2], 'outputs': [3]},  # Conv2D
        {'opcode_index': 1, 'inputs': [3, 4], 'outputs': [5]},     # MEAN
    ]

    op_codes = [3, 25]  # CONV_2D=3, MEAN=25

    data = build_tflite_model_raw(
        tensors, operators, [0], [5],
        op_codes, buffers, "screen_classifier_placeholder"
    )

    with open(output_path, 'wb') as f:
        f.write(data)

    return len(data)


def generate_ui_detector(output_path):
    """
    ui_detector.tflite を生成
    入力: [1, 320, 320, 3] float32
    出力: SSD-like 4出力
      - [1, 50, 4] float32 (boxes)
      - [1, 50] float32 (scores)
      - [1, 50] float32 (classes)
      - [1] float32 (num_detections)

    構造: Conv2D(3x3, 16) -> Conv2D(1x1, 出力) -> Reshape x4
    """
    # Conv2D weights: [16, 3, 3, 3] = 432 floats
    conv1_weights = np.zeros((16, 3, 3, 3), dtype=np.float32)
    for i in range(16):
        conv1_weights[i, 1, 1, i % 3] = 0.01
    conv1_weight_data = conv1_weights.tobytes()
    conv1_bias = np.zeros(16, dtype=np.float32)
    conv1_bias_data = conv1_bias.tobytes()

    # 2nd Conv2D: [250, 1, 1, 16] (50*4 + 50 = 250 outputs for boxes+scores)
    # 簡略化: 1つのConvで250チャネル出力し、Reshapeで分割
    conv2_weights = np.zeros((250, 1, 1, 16), dtype=np.float32)
    conv2_weight_data = conv2_weights.tobytes()
    conv2_bias = np.zeros(250, dtype=np.float32)
    conv2_bias_data = conv2_bias.tobytes()

    # Reshape用 new_shape テンソル
    boxes_shape = np.array([1, 50, 4], dtype=np.int32)
    scores_shape = np.array([1, 50], dtype=np.int32)
    classes_shape = np.array([1, 50], dtype=np.int32)
    ndet_shape = np.array([1], dtype=np.int32)

    # num_detections用の定数
    num_det_data = np.array([0.0], dtype=np.float32)  # 0 detections (placeholder)

    # dummy scores/classes (all zeros)
    dummy_scores = np.zeros(50, dtype=np.float32)
    dummy_classes = np.zeros(50, dtype=np.float32)
    dummy_boxes = np.zeros(200, dtype=np.float32)  # 50*4

    buffers = [
        None,                    # 0: 空（入力）
        conv1_weight_data,       # 1: conv1 weights
        conv1_bias_data,         # 2: conv1 bias
        None,                    # 3: 空（conv1出力）
        conv2_weight_data,       # 4: conv2 weights
        conv2_bias_data,         # 5: conv2 bias
        None,                    # 6: 空（conv2出力）
        boxes_shape.tobytes(),   # 7: boxes reshape target
        scores_shape.tobytes(),  # 8: scores reshape target
        classes_shape.tobytes(), # 9: classes reshape target
        ndet_shape.tobytes(),    # 10: num_det reshape target
        dummy_boxes.tobytes(),   # 11: dummy boxes output
        dummy_scores.tobytes(),  # 12: dummy scores output
        dummy_classes.tobytes(), # 13: dummy classes output
        num_det_data.tobytes(),  # 14: num_detections output
    ]

    tensors = [
        # 入力
        {'name': 'input', 'shape': [1, 320, 320, 3], 'type': 0, 'buffer': 0},         # 0
        # Conv1
        {'name': 'conv1_w', 'shape': [16, 3, 3, 3], 'type': 0, 'buffer': 1},          # 1
        {'name': 'conv1_b', 'shape': [16], 'type': 0, 'buffer': 2},                    # 2
        {'name': 'conv1_out', 'shape': [1, 318, 318, 16], 'type': 0, 'buffer': 3},     # 3
        # Conv2
        {'name': 'conv2_w', 'shape': [250, 1, 1, 16], 'type': 0, 'buffer': 4},         # 4
        {'name': 'conv2_b', 'shape': [250], 'type': 0, 'buffer': 5},                   # 5
        {'name': 'conv2_out', 'shape': [1, 318, 318, 250], 'type': 0, 'buffer': 6},    # 6
        # 出力テンソル（定数として保持）
        {'name': 'boxes', 'shape': [1, 50, 4], 'type': 0, 'buffer': 11},               # 7
        {'name': 'scores', 'shape': [1, 50], 'type': 0, 'buffer': 12},                 # 8
        {'name': 'classes', 'shape': [1, 50], 'type': 0, 'buffer': 13},                # 9
        {'name': 'num_detections', 'shape': [1], 'type': 0, 'buffer': 14},             # 10
    ]

    # Conv2D opcode only (出力は定数バッファとして直接保持)
    operators = [
        {'opcode_index': 0, 'inputs': [0, 1, 2], 'outputs': [3]},  # Conv2D 1
        {'opcode_index': 0, 'inputs': [3, 4, 5], 'outputs': [6]},  # Conv2D 2
    ]

    op_codes = [3]  # CONV_2D=3

    data = build_tflite_model_raw(
        tensors, operators, [0], [7, 8, 9, 10],
        op_codes, buffers, "ui_detector_placeholder"
    )

    with open(output_path, 'wb') as f:
        f.write(data)

    return len(data)


def main():
    output_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                              'android', 'capture', 'src', 'main', 'assets', 'models')
    os.makedirs(output_dir, exist_ok=True)

    print("=" * 60)
    print("TFLite Placeholder Model Generator")
    print("=" * 60)

    # 1. screen_classifier.tflite
    path1 = os.path.join(output_dir, 'screen_classifier.tflite')
    size1 = generate_screen_classifier(path1)
    print(f"\n[OK] screen_classifier.tflite")
    print(f"     Path: {path1}")
    print(f"     Size: {size1:,} bytes ({size1/1024:.1f} KB)")
    print(f"     Input:  [1, 224, 224, 3] float32")
    print(f"     Output: [1, 8] float32")

    # 2. ui_detector.tflite
    path2 = os.path.join(output_dir, 'ui_detector.tflite')
    size2 = generate_ui_detector(path2)
    print(f"\n[OK] ui_detector.tflite")
    print(f"     Path: {path2}")
    print(f"     Size: {size2:,} bytes ({size2/1024:.1f} KB)")
    print(f"     Input:  [1, 320, 320, 3] float32")
    print(f"     Output: [1,50,4] boxes, [1,50] scores, [1,50] classes, [1] num_det")

    print(f"\n{'=' * 60}")
    print(f"Total: {(size1+size2):,} bytes ({(size1+size2)/1024:.1f} KB)")
    print(f"{'=' * 60}")

    # 検証: ファイルヘッダをチェック
    for name, path in [("screen_classifier", path1), ("ui_detector", path2)]:
        with open(path, 'rb') as f:
            data = f.read(8)
            # FlatBuffer offset (4 bytes) + file_identifier "TFL3" (4 bytes)
            file_id = data[4:8]
            if file_id == b'TFL3':
                print(f"[VERIFY OK] {name}: TFL3 header confirmed")
            else:
                print(f"[VERIFY NG] {name}: unexpected header {file_id}")
                return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
