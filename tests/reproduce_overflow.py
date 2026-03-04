#################################################################
# Test Script: reproduce_overflow.py
# Purpose: To verify if writing > 4GB to MemoryStream causes size overflow in chunk_data_t.size (u32).
# Usage: python reproduce_overflow.py [size_in_gb]
# Note: This test can consume a lot of RAM due to C++ std::vector resizing strategy. Use with caution.
#################################################################
import sys
import os
import time

try:
    from fastdb4py import core
except ImportError:
    # Try local import if package is installed in weird way or running from source
    # adjust path as needed
    sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '../../python')))
    try:
        from fastdb4py import core
    except ImportError:
        print("Error: Could not import fastdb4py.core")
        sys.exit(1)

def run_test(target_size_gb=4.05):
    """
    Tries to write > 4GB data to MemoryStream to verify if size overflows 32-bit integer.
    target_size_gb: The size to write in GB. Should be > 4.0 to trigger overflow.
    """
    target_bytes = int(target_size_gb * 1024 * 1024 * 1024)
    
    # Use 64MB chunks to avoid one giant python allocation
    chunk_size = 64 * 1024 * 1024  
    chunk = b'\x00' * chunk_size
    
    print(f"[-] Allocating ~{target_size_gb:.2f} GB in MemoryStream...")
    print("[-] WARNING: This test can consume > 8GB of RAM due to C++ std::vector resizing strategy.")
    print("[-] If your machine has < 8GB RAM, this script might be killed by OOM killer.")
    
    ms = core.WxMemoryStream()
    total_written = 0
    start_time = time.time()
    
    try:
        while total_written < target_bytes:
            remaining = target_bytes - total_written
            
            # Determine how much to write in this iteration
            current_write_size = min(remaining, chunk_size)
            
            # Slice buffer if needed (avoid copy if possible, memoryview might be better but bytes slice works for now)
            bytes_to_write = chunk if current_write_size == chunk_size else chunk[:current_write_size]
            
            # Depending on SWIG typemap, write might take 1 or 2 args.
            # Based on inspection, it likely takes 1 argument (buffer) due to %typemap(in) (void* pdata, size_t size)
            try:
                ms.write(bytes_to_write)
            except TypeError:
                # Fallback if typemap doesn't match single arg
                ms.write(bytes_to_write, len(bytes_to_write))
                
            total_written += current_write_size
            
            # Print progress
            if total_written % (512 * 1024 * 1024) < chunk_size:
                 elapsed = time.time() - start_time
                 speed = (total_written / (1024**3)) / elapsed if elapsed > 0 else 0
                 print(f"    Written: {total_written / (1024**3):.2f} GB ({speed:.2f} GB/s)")

    except MemoryError:
        print("\n[ERROR] Out of Memory! Python failed to allocate memory.")
        sys.exit(1)
    except Exception as e:
        print(f"\n[ERROR] Unexpected error during write: {e}")
        # Even if we crash, we might want to check what we wrote so far
        pass

    # Verify result
    print(f"\n[-] Finished writing. Total intended: {total_written} bytes")
    
    try:
        result = ms.data()
        reported_size = result.size # This is the u32 field from C++ struct
        
        print(f"[-] Reported Size (from chunk_data_t.size): {reported_size} bytes")
        
        expected_modulo = total_written % (2**32)
        
        if reported_size != total_written:
            print(f"[FAIL] Size mismatch! {reported_size} != {total_written}")
            if reported_size == expected_modulo:
                diff = total_written - reported_size
                print(f"[FAIL] Bug Reproduced: Size overflowed exactly by {diff} bytes (approx {diff/1024/1024/1024:.0f}GB).")
                print(f"       This confirms that chunk_data_t.size is truncated to 32-bit unsigned integer.")
                
                # Check for Magic String issue
                # If we wrote > 4GB, the reported size is small (modulo).
                # If the modulo is very small (e.g. < 16 bytes), the magic string (first 16 bytes) is effectively lost
                # because the system thinks the file is only N bytes long.
                if reported_size < 16:
                     print("[CRITICAL] Magic string compromised! File size is reported as < 16 bytes.")
                else:
                     print("[WARN] Magic string might be present in first 16 bytes, but file is truncated.")
            else:
                print(f"[FAIL] Size mismatch but not exact modulo match. Got {reported_size}")
        else:
            print("[PASS] Size matches correctly (No overflow observed).")
            print("       (Did you write enough data? You typically need > 4GB to trigger this.)")
            
    except Exception as e:
        print(f"[ERROR] Failed to inspect result: {e}")

if __name__ == "__main__":
    size_gb = 4.05 # Default to slightly over 4GB
    if len(sys.argv) > 1:
        try:
            size_gb = float(sys.argv[1])
        except ValueError:
            print("Usage: python reproduce_overflow.py [size_in_gb]")
            sys.exit(1)
            
    run_test(size_gb)
