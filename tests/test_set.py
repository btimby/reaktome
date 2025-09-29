import unittest

from reaktome import reaktiv8


class SetTestCase(unittest.TestCase):
    def setUp(self):
        self.set = set()
        reaktiv8(self.set)

    def test_add(self):
        self.set.add(1)
