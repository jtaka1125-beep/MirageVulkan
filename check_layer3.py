with open('src/ai_engine.cpp', 'r', encoding='utf-8') as f:
    lines=f.readlines()
for i, l in enumerate(lines,1):
    if 'Layer3' in l or 'layer3' in l:
        print(i, repr(l[:80]))
