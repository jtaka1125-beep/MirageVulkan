import re, sys, os

def remove_layer3(path):
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        # Skip #include ollama_vision.hpp
        if '#include "ollama_vision.hpp"' in line:
            i += 1
            continue

        # Skip entire functions related to Layer3
        layer3_funcs = [
            'setOllamaVision', 'isLayer3OnCooldown', 'isLayer3Running',
            'launchLayer3Async', 'pollLayer3Result', 'cancelLayer3',
            'shouldTriggerLayer3', 'checkLayer3Freeze'
        ]
        if any(f in line for f in layer3_funcs):
            # Skip entire function body
            brace = 0
            found_open = False
            while i < len(lines):
                for ch in lines[i]:
                    if ch == '{': brace += 1; found_open = True
                    elif ch == '}': brace -= 1
                i += 1
                if found_open and brace == 0:
                    break
            # Remove preceding comment lines
            while out and out[-1].strip().startswith('//'):
                out.pop()
            continue

        # Skip inline layer3 state references
        layer3_keywords = [
            'layer3_last_call', 'layer3_start_time', 'layer3_task',
            'layer3_active_count_', 'LAYER3_MAX_CONCURRENT'
        ]
        if any(k in line for k in layer3_keywords):
            i += 1
            continue

        out.append(line)
        i += 1

    with open(path, 'w', encoding='utf-8') as f:
        f.writelines(out)

    return len(lines), len(out)

before, after = remove_layer3('src/ai/vision_decision_engine.cpp')
print(f'vde.cpp: {before} -> {after} lines')
sys.stdout.flush()
