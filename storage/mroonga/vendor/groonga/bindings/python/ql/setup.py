#!/usr/bin/python
from distutils.core import setup, Extension

#cflags = ['-Wall', '-fPIC', '-g', '-O0']
cflags = ['-Wall', '-fPIC', '-O3']

ext = Extension('groongaql',
                libraries = ['groonga'],
                sources = ['groongaql.c'],
                extra_compile_args = cflags)

setup(name = 'groongaql',
      version = '1.0',
      description = 'python GQTP',
      long_description = '''
      This is a GQTP Python API package.
      ''',
      license='GNU LESSER GENERAL PUBLIC LICENSE',
      author = 'Brazil',
      author_email = 'groonga at razil.jp',
      ext_modules = [ext]
     )
