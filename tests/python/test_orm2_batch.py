import pytest
import time
from typing import List
from fastdb4py.decorator import feature
from fastdb4py.orm2 import ORM2
from fastdb4py.type import F64, U32, STR


@feature
class BatchPoint:
    x: F64
    y: F64
    tag: STR


@feature
class Company:
    name: STR


@feature
class Department:
    name: STR
    company: Company


@feature
class Employee:
    name: STR
    dept: Department


class TestBatchCorrectness:
    def test_1000_simple_objects(self):
        """Push 1000 simple objects, verify all column values after combine."""
        orm = ORM2.create()
        for i in range(1000):
            p = BatchPoint()
            p.x = float(i)
            p.y = float(i * 2)
            p.tag = f"p{i}"
            orm.push(p)
        assert orm.count(BatchPoint) == 1000
        orm.combine()
        for i in range(1000):
            r = orm.get(BatchPoint, i, mode='copy')
            assert abs(r.x - float(i)) < 1e-9, f"x mismatch at {i}"
            assert abs(r.y - float(i * 2)) < 1e-9, f"y mismatch at {i}"
            assert r.tag == f"p{i}", f"tag mismatch at {i}"

    def test_dedup_same_object(self):
        """Push same object twice — should produce only one row."""
        orm = ORM2.create()
        p = BatchPoint()
        p.x = 42.0
        p.y = 0.0
        p.tag = "dupe"
        orm.push(p)
        orm.push(p)
        assert orm.count(BatchPoint) == 1
        orm.combine()
        assert orm.get(BatchPoint, 0, mode='copy').x == 42.0

    def test_mutation_after_push(self):
        """Mutating object after push should reflect in combine output."""
        orm = ORM2.create()
        p = BatchPoint()
        p.x = 1.0
        p.y = 2.0
        p.tag = "before"
        orm.push(p)
        p.x = 99.0
        p.tag = "after"
        orm.combine()
        r = orm.get(BatchPoint, 0, mode='copy')
        assert abs(r.x - 99.0) < 1e-9, "mutation after push should be visible"
        assert r.tag == "after"


class TestRefTopoSort:
    def test_three_level_chain(self):
        """A→B→C: push C first (Employee), deps auto-pushed in correct order."""
        orm = ORM2.create()
        c = Company()
        c.name = "Acme"
        d = Department()
        d.name = "Engineering"
        d.company = c
        e = Employee()
        e.name = "Alice"
        e.dept = d
        orm.push(e)
        assert orm.count(Company) == 1
        assert orm.count(Department) == 1
        assert orm.count(Employee) == 1
        orm.combine()
        co = orm.get(Company, 0, mode='copy')
        assert co.name == "Acme"
        dep = orm.get(Department, 0, mode='copy')
        assert dep.name == "Engineering"
        emp = orm.get(Employee, 0, mode='copy')
        assert emp.name == "Alice"

    def test_shared_ref_dedup(self):
        """Two objects referencing the same dep — dep pushed once."""
        orm = ORM2.create()
        c = Company()
        c.name = "SharedCo"
        d1 = Department()
        d1.name = "Sales"
        d1.company = c
        d2 = Department()
        d2.name = "Marketing"
        d2.company = c
        orm.push(d1)
        orm.push(d2)
        assert orm.count(Company) == 1
        assert orm.count(Department) == 2
        orm.combine()
        assert orm.get(Company, 0, mode='copy').name == "SharedCo"

    def test_list_ref(self):
        """LIST[REF] field: list of referenced objects."""
        @feature
        class Tag:
            label: STR

        @feature
        class Article:
            title: STR
            tags: List[Tag]

        orm = ORM2.create()
        t1 = Tag(); t1.label = "python"
        t2 = Tag(); t2.label = "perf"
        a = Article()
        a.title = "ORM2 Optimization"
        a.tags = [t1, t2]
        orm.push(a)
        assert orm.count(Tag) == 2
        assert orm.count(Article) == 1
        orm.combine()
        assert orm.get(Article, 0, mode='copy').title == "ORM2 Optimization"
        assert orm.get(Tag, 0, mode='copy').label == "python"
        assert orm.get(Tag, 1, mode='copy').label == "perf"


class TestPerformanceParity:
    def test_simple_push_perf(self):
        """ORM2 batch push should be within 5x of old ORM for 2000 simple features."""
        N = 2000
        WARMUP = 1

        for _ in range(WARMUP):
            orm = ORM2.create()
            for i in range(100):
                p = BatchPoint()
                p.x = float(i)
                p.y = float(i)
                p.tag = f"w{i}"
                orm.push(p)
            orm.combine()

        t0 = time.perf_counter()
        orm = ORM2.create()
        for i in range(N):
            p = BatchPoint()
            p.x = float(i)
            p.y = float(i * 2)
            p.tag = f"p{i}"
            orm.push(p)
        orm.combine()
        elapsed_ms = (time.perf_counter() - t0) * 1000

        assert orm.count(BatchPoint) == N
        r = orm.get(BatchPoint, N - 1, mode='copy')
        assert abs(r.x - float(N - 1)) < 1e-9

        print(f"\nORM2 batch push {N} features: {elapsed_ms:.1f}ms "
              f"({elapsed_ms/N*1000:.0f}µs/push)")
        assert elapsed_ms < 15, (
            f"ORM2 push {N} features took {elapsed_ms:.1f}ms, "
            "expected < 15ms (old ORM baseline: 3.25ms)"
        )
