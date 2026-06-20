# vapoursynth-autocrop
AutoCrop for Vapoursynth  

# Usage  
AutoCrop  
```python
acrop.AutoCrop(clip src, int[] max_crop=16, int[] ref_color=[0,127,127], float[] max_color_deviation=0.0, int[] pad=0, int[] mod=2, int roundup=False)
```  

Search only  
```python
acrop.CropValues(clip src, int[] max_crop=16, int[] ref_color=[0,127,127], float[] max_color_deviation=0.0, int[] pad=0, int[] mod=2, int roundup=False, int debug=False)
```

Detected crop values are stored as integer frame properties named `CropLeftValue`, `CropRightValue`, `CropTopValue`, `CropBottomValue`.

When `debug=True`, `CropValues` also stores the input frame's edge midpoint colors in the integer-array frame properties `DebugLeftColor`, `DebugRightColor`, `DebugTopColor`, and `DebugBottomColor`. The sampled luma coordinates are `(0, height / 2)`, `(width - 1, height / 2)`, `(width / 2, 0)`, and `(width / 2, height - 1)`, using integer division. Each array contains one native sample value per plane in plane order, such as Y, U, V for YUV. Chroma coordinates are shifted according to the format's subsampling, and values retain the input clip's native bit depth.

`max_crop` limits how far the border search can scan into the frame. It accepts either one value for all sides or four values in left, right, top, bottom order. Values must be compatible with the clip subsampling.

`ref_color` uses 8-bit-style sample values and is scaled to the bit depth of the input clip. It accepts either one value or one value per clip plane. On YUV and YCoCg clips, a single value sets the luma plane and uses neutral chroma values of 127. On GRAY clips, only the first plane is used.

`max_color_deviation` is normalized to the valid sample range of the input format and accepts either one value for all active planes or one value per clip plane. Values must be in the 0.0-1.0 range. The native sample tolerance is calculated with `round(max_color_deviation * ((1 << bits_per_sample) - 1))`, then the final min and max pixel values are clamped to the valid sample range.

For example, `max_color_deviation=0.04` on an 8-bit clip calculates a tolerance of `round(0.04 * 255) = 10`. With the default black reference color, this matches Y values from 0 to 10 and chroma values from 117 to 137.

`pad` adds pixels to the analyzed crop values, cropping more aggressively. `mod` is applied after `pad` and limits crop values to numbers divisible by the specified value. Both parameters accept either one value for all sides or four values in left, right, top, bottom order. By default, `mod` rounds crop values down. Set `roundup=True` to round crop values up before the final valid-size clamp.

For example, `pad=4` adds 4 pixels to all crop sides. `pad=[1, 2, 3, 4]` uses left=1, right=2, top=3, bottom=4. `mod=4` rounds all adjusted crop values down to multiples of 4, while `mod=[2, 4, 2, 4]` sets each side separately.

## Compilation

### Linux
```
g++ -std=c++11 -shared -fPIC -O2 autocrop.cpp -o libautocrop.so
```

### Cross-compilation for Windows
```
x86_64-w64-mingw32-g++ -std=c++11 -shared -fPIC -O2 autocrop.cpp -static-libgcc -static-libstdc++ -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic -s -o autocrop.dll
```


# Thanks  
kageru, Attila, stux!, and Myrsloik
