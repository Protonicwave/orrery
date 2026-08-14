"""The service's tests, as a package.

A package rather than a bare directory so that the cases can import the marks
and fixtures in `conftest.py` by name. pytest makes a conftest's fixtures
available without an import, but not the two skip marks, and a mark applied by
hand in each module is what states in the module itself what it needs to run.
"""
