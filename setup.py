import os
import sys
import subprocess
import shutil
from setuptools import setup, Extension, find_packages
from setuptools.command.build_ext import build_ext

class CMakeExtension(Extension):
    def __init__(self, name, sourcedir=''):
        Extension.__init__(self, name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir)

class CMakeBuild(build_ext):
    def run(self):
        try:
            subprocess.check_output(['cmake', '--version'])
        except OSError:
            raise RuntimeError("CMake not found. Ensure it is in pyproject.toml requires")
        
        for ext in self.extensions:
            self.build_extension(ext)

    def build_extension(self, ext):
        extdir = os.path.abspath(os.path.dirname(self.get_ext_fullpath(ext.name)))
        
        cmake_args = [
            f'-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={extdir}',
            f'-DPython3_EXECUTABLE={sys.executable}',
            '-DUSE_SWIG_PYTHON=ON',
            '-DCMAKE_BUILD_TYPE=Release',
            '-DBUILD_TOOLS=OFF',
        ]

        if not os.path.exists(self.build_temp):
            os.makedirs(self.build_temp)

        subprocess.check_call(['cmake', ext.sourcedir + '/fastcarto'] + cmake_args, cwd=self.build_temp)
        subprocess.check_call(['cmake', '--build', '.', '--config', 'Release', '--parallel'], cwd=self.build_temp)
        
        cmake_out_dir = os.path.join(ext.sourcedir, 'python', 'fastdb', 'core')
        dest_dir = os.path.join(self.build_lib, 'fastdb', 'core')
        
        self.copy_tree(cmake_out_dir, dest_dir)

    def copy_tree(self, src, dst):
        if not os.path.exists(dst):
            os.makedirs(dst)
        for item in os.listdir(src):
            s = os.path.join(src, item)
            d = os.path.join(dst, item)
            if os.path.isfile(s):
                if any(s.endswith(ext) for ext in ['.so', '.pyd', '.dll', '.dylib', '.py']):
                    shutil.copy2(s, d)

setup(
    name='fastdb',
    packages=find_packages(where='python'),
    package_dir={'': 'python'},
    ext_modules=[CMakeExtension('fastdb.core._fastdb4py', sourcedir='.')],
    cmdclass=dict(build_ext=CMakeBuild),
    zip_safe=False,
)