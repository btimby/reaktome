from setuptools import setup

from reaktome.__version__ import __version__


setup(
    name='reaktome',
    version=__version__,
    author='Ben Timby',
    author_email='btimby@gmail.com',
    description='Vue-like reactivity for Python',
    long_description='Track and report changes to object hierarchy.',
    packages=['reaktome'],
    classifiers=[
        'Development Status :: 5 - Production/Stable',
        'Intended Audience :: Developers',
        'License :: OSI Approved :: MIT License',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python :: 3.6',
    ],
)