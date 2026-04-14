import pytest
from fastdb4py.decorator import feature
from fastdb4py.orm2 import ORM2
from fastdb4py.type import F64, STR


@feature
class SharedPoint:
    x: F64
    y: F64
    label: STR


def test_share_and_load():
    orm = ORM2.create()
    for i in range(5):
        p = SharedPoint()
        p.x = float(i)
        p.y = float(i * 2)
        p.label = f"p{i}"
        orm.push(p)
    orm.combine()

    shm_name = "test_orm2_share"
    try:
        orm.share(shm_name)

        orm2 = ORM2.load(shm_name)
        assert orm2.count(SharedPoint) == 5

        result = orm2.get(SharedPoint, 3, mode='copy')
        assert abs(result.x - 3.0) < 1e-9
        assert result.label == "p3"
    finally:
        ORM2.unlink(shm_name)


def test_share_empty_database():
    """Test sharing an empty database."""
    orm = ORM2.create()
    orm.combine()

    shm_name = "test_orm2_empty"
    try:
        orm.share(shm_name)
        
        orm2 = ORM2.load(shm_name)
        assert orm2.count(SharedPoint) == 0
    finally:
        ORM2.unlink(shm_name)


def test_share_before_combine_fails():
    """Test that sharing fails if combine() hasn't been called."""
    orm = ORM2.create()
    p = SharedPoint()
    p.x = 1.0
    p.y = 2.0
    p.label = "test"
    orm.push(p)

    with pytest.raises(RuntimeError, match="Call combine\\(\\) before sharing"):
        orm.share("test_orm2_fail")


def test_load_nonexistent_fails():
    """Test that loading from non-existent shared memory fails."""
    with pytest.raises(FileNotFoundError):
        ORM2.load("nonexistent_shm")


def test_unlink_nonexistent_is_safe():
    """Test that unlinking non-existent shared memory doesn't crash."""
    ORM2.unlink("nonexistent_shm")  # Should not raise


def test_load_preserves_layer_structure():
    """Test that loading properly reconstructs layer structure."""
    orm = ORM2.create()
    
    # Add features with different types to create multiple layers
    for i in range(3):
        p = SharedPoint()
        p.x = float(i * 10)
        p.y = float(i * 100)
        p.label = f"point_{i}"
        orm.push(p)
    
    orm.combine()
    
    shm_name = "test_orm2_structure"
    try:
        orm.share(shm_name)
        
        orm2 = ORM2.load(shm_name)
        
        # Check that layer structure is preserved
        assert orm2.count(SharedPoint) == 3
        
        # Check iteration works
        results = list(orm2.iter(SharedPoint, mode='copy'))
        assert len(results) == 3
        
        # Check specific values
        result = orm2.get(SharedPoint, 1, mode='copy')
        assert abs(result.x - 10.0) < 1e-9
        assert abs(result.y - 100.0) < 1e-9
        assert result.label == "point_1"
    finally:
        ORM2.unlink(shm_name)