"""
Plugin hooks for the MetaFFI JVM plugin.

Invoked by the CLI installer:
  python plugin_hooks.py --check-prerequisites
  python plugin_hooks.py --setup-environment
  python plugin_hooks.py --pre-uninstall
"""

import os
import platform
import subprocess
import sys


def check_prerequisites() -> bool:
	"""Return True if prerequisites met. Print message and return False if not."""

	try:
		completed = subprocess.run(
			['java', '-version'],
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			text=True
		)
		if completed.returncode != 0:
			print("java is not installed")
			return False

		combined = completed.stdout + completed.stderr
		if 'version "21.' in combined.lower():
			return True
		else:
			print(f"JVM 21 is required\nfound:{combined}")
			return False

	except Exception:
		print("java is not installed")
		return False


def setup_environment():
	"""Called after files are installed. Set env vars, etc."""

	try:
		import pycrosskit
	except ImportError:
		print("pycrosskit for writing environment variables is missing")
		print("make sure to install requirements.txt and try again")
		sys.exit(1)

	from pycrosskit.envariables import SysEnv

	java_home = os.environ.get('JAVA_HOME')
	if java_home is None:
		raise EnvironmentError("JAVA_HOME is not set. Cannot configure library paths.")

	if platform.system() == 'Windows':
		# Add $JAVA_HOME/bin/server to PATH
		server_path = f'{java_home}/bin/server'
		current_path = os.environ.get('PATH', '')
		if server_path not in current_path:
			SysEnv().set('PATH', f'{server_path};{current_path}')
	else:
		# Add $JAVA_HOME/lib/server to LD_LIBRARY_PATH
		server_path = f'{java_home}/lib/server'
		ld_lib = os.environ.get('LD_LIBRARY_PATH', '')
		paths = ld_lib.split(':') if ld_lib else []
		if server_path not in paths:
			new_ld = f'{server_path}:{ld_lib}' if ld_lib else server_path
			SysEnv().set('LD_LIBRARY_PATH', new_ld)


def pre_uninstall():
	"""Called before plugin directory is removed. Clean up env vars, etc."""

	# JVM plugin currently has no environment cleanup needed
	pass


if __name__ == "__main__":
	if len(sys.argv) < 2:
		print("Usage: python plugin_hooks.py --check-prerequisites|--setup-environment|--pre-uninstall")
		sys.exit(1)

	action = sys.argv[1]

	if action == '--check-prerequisites':
		ok = check_prerequisites()
		sys.exit(0 if ok else 1)

	elif action == '--setup-environment':
		setup_environment()
		sys.exit(0)

	elif action == '--pre-uninstall':
		pre_uninstall()
		sys.exit(0)

	else:
		print(f"Unknown action: {action}")
		sys.exit(1)
