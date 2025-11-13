from enum import unique, IntEnum

# A map of the enum type used in the core fastdb library
@unique
class OriginFieldType(IntEnum):
    unknown     = 0
    u8          = 1
    u16         = 2
    u32         = 3
    i32         = 4
    u8n         = 5
    u16n        = 6
    f32         = 7
    f64         = 8
    str         = 9
    wstr        = 10
    ref         = 11
    bytes       = 12
    list        = 13
    
    def __repr__(self):
        return self.name
