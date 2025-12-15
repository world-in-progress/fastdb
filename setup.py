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

        if sys.platform == 'darwin':
            # Handle macOS architecture flags set by cibuildwheel (e.g. ARCHFLAGS="-arch x86_64")
            archflags = os.environ.get("ARCHFLAGS", "")
            if archflags:
                archs = []
                if "arm64" in archflags:
                    archs.append("arm64")
                if "x86_64" in archflags:
                    archs.append("x86_64")
                if archs:
                    cmake_args.append(f'-DCMAKE_OSX_ARCHITECTURES={";".join(archs)}')

        # Limit parallel jobs to avoid OOM on Docker with limited memory
        # Default to 4 or CPU count, whichever is smaller, to be safe
        import multiprocessing
        try:
            num_jobs = min(2, multiprocessing.cpu_count())
        except NotImplementedError:
            num_jobs = 1
            
        # Ensure the temporary build directory exists before invoking CMake
        if not os.path.exists(self.build_temp):
            os.makedirs(self.build_temp, exist_ok=True)

        # Ensure extension output directory exists so CMake can write artifacts
        if not os.path.exists(extdir):
            os.makedirs(extdir, exist_ok=True)

        if sys.platform == 'win32':
            cmake_args += ['-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE=' + extdir]
            cmake_args += ['-A', 'x64']

        subprocess.check_call(['cmake', ext.sourcedir + '/fastcarto'] + cmake_args, cwd=self.build_temp)
        subprocess.check_call(['cmake', '--build', '.', '--config', 'Release', '--parallel', str(num_jobs)], cwd=self.build_temp)
        
        cmake_out_dir = os.path.join(ext.sourcedir, 'python', 'fastdb4py', 'core')
        dest_dir = os.path.join(self.build_lib, 'fastdb4py', 'core')
        
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
    name='fastdb4py',
    packages=find_packages(where='python'),
    package_dir={'': 'python'},
    ext_modules=[CMakeExtension('fastdb4py.core._fastdb4py', sourcedir='.')],
    cmdclass=dict(build_ext=CMakeBuild),
    zip_safe=False,
)