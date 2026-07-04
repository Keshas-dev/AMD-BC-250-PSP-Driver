import os, re

in_file = r"C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\inc\firmware_data.h"
out_dir = r"C:\AMD-BC-250\AMD-BC-250-PSP-Windows-Driver\output\firmware"
os.makedirs(out_dir, exist_ok=True)

current_name = None
current_data = []
name_pattern = re.compile(r'static const UCHAR g_(\w+)FirmwareData\[\]\s*=')

with open(in_file, 'r') as f:
    for line in f:
        # Check for start of a firmware array
        m = name_pattern.search(line)
        if m:
            current_name = m.group(1)
            current_data = []
            # Parse inline data on same line
            brace = line.find('{')
            close = line.find('};')
            if brace >= 0 and close > brace:
                segment = line[brace+1:close]
                current_data.append(segment)
                # Write file inline
                hex_str = ''.join(current_data)
                vals = re.findall(r'0[xX]([0-9a-fA-F]{2})', hex_str)
                if vals:
                    byte_data = bytes(int(h, 16) for h in vals)
                    out_path = os.path.join(out_dir, f"{current_name}.bin")
                    with open(out_path, 'wb') as fout:
                        fout.write(byte_data)
                    print(f"  {current_name}: {len(byte_data)} bytes (inline)")
                current_name = None
                current_data = []
            continue
        
        if current_name:
            # Check for end of array
            close_idx = line.find('};')
            if close_idx >= 0:
                segment = line[:close_idx]
                current_data.append(segment)
                # Write file
                hex_str = ''.join(current_data)
                vals = re.findall(r'0[xX]([0-9a-fA-F]{2})', hex_str)
                if vals:
                    byte_data = bytes(int(h, 16) for h in vals)
                    out_path = os.path.join(out_dir, f"{current_name}.bin")
                    with open(out_path, 'wb') as fout:
                        fout.write(byte_data)
                    print(f"  {current_name}: {len(byte_data)} bytes")
                current_name = None
                current_data = []
            else:
                current_data.append(line.strip())

print("Done.")
