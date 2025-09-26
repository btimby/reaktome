import unittest

from typing import Any

from unittest.mock import MagicMock
from pydantic import BaseModel
from pydantic_collections import BaseCollectionModel

from reaktome import Reaktome, ReaktomeWatch, reaktiv8


class ReaktomeCollection(ReaktomeWatch):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        for i, obj in enumerate(self):
            # Don't double wrap...
            super().__setitem__(i, reaktiv8(obj, on_change=self.on_change, path=f'[{i}]'))

    def __setitem__(self, name, value):
        old = self[name]
        super().__setitem__(name, reaktiv8(value, on_change=self.on_change, path=f'[{name}]'))
        self.on_change(f'[{name}]', old, value)


class FooModel(BaseModel):
    id: str
    name: str


class BarModel(BaseModel):
    id: str
    name: str


class FooModelCollection(ReaktomeCollection, BaseCollectionModel[FooModel]):
    pass


class BarModelCollection(ReaktomeCollection, BaseCollectionModel[BarModel]):
    pass


class ReaktomeTestCase(unittest.TestCase):
    def test_reaktome_collection(self):
        handler = MagicMock()
        coll = FooModelCollection([
            {'id': 'abc123', 'name': 'Roger'},
        ])
        coll.on('*', handler)
        coll[0] = FooModel(id='xyz987', name='Richard')
        coll[0].id = 'efg456'
        self.assertEqual(2, handler.call_count)

    def test_reaktome_isinstance(self):
        coll = FooModelCollection([
            {'id': 'abc123', 'name': 'Roger'},
        ])
        import pdb; pdb.set_trace()
        self.assertTrue(type(coll[0]) == FooModel)