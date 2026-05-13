#  Axivion Suite
#  Copyright (C) 2022-2025 Axivion GmbH
#  Copyright (C) 2025 The Qt Company GmbH, a subsidiary of The Qt Group
#  https://www.qt.io


"""Utility functions for the creation and processing of hierarchies.

The functions provided by this module are primarily used to set up the
backing data of tree-based visualizations, such as TreeMaps, Circle
Packing, Hierarchical Edge Bundles etc.

"""

from dataclasses import dataclass
from typing import List, MutableMapping, Sequence, Union

# Represents a tree of files and directories containing other
# files. The hierarchy is encoded as a dict of dicts where either:
#
#   1) The key is the directory name, `d`, and the value is a dict
#      containing the subdirectories and files located in `d`.
#
# or:
#
#   2) The key is the file name and the value is the entire path of
#      this particular file.
PathTree = MutableMapping[str, Union['PathTree', str]]


def build_tree_from_file_paths(
    paths: Sequence[str], name_of_root: str = '<root>'
) -> PathTree:
    """Given a sequence of file paths, this function computes a tree
       representing the file hierarchy. Note that, by design, the
       returned hierarchy cannot have empty directories. Yet, if the
       supplied sequence is empty, the returned hierarchy is also
       empty.

    The directory structure is computed by splitting the input paths
    at delimiter `/`---i.e., the path separator used is `/`. This
    function also handles the path separator `\\` (which is typically
    used on Windows systems) by replacing all instances of `\\` with
    `/` prior to the processing of the input paths. If `\\` is a
    regular character used in file and directory names, it is up to
    the caller of this function to maintain these character instances
    (e.g., by replacing them with `:` before calling this function and
    resetting them afterwards). Regardless of whether the input paths
    are separated with `/` or `\\`, the leaf nodes of the returned
    tree always have separator `/`.

    If `paths` is empty, the returned tree is empty.

    If the resulting tree would have multiple root nodes, `rs`, an
    artificial root node with name `<root>` is added to the tree,
    subsuming `rs`. The name of the artificial root node can be set
    through the parameter `name_of_root` (its default value is
    `<root>`). The artificial root node is not prepended to the path
    of the files of the returned hierarchy.

    Parameters:
    paths        -- sequence of files from which the file hierarchy is
                    built up
    name_of_root -- name of the artificial root node

    """

    def _helper(path: str, leaf: str, tree: PathTree) -> None:
        parts = path.split('/', 1)
        if len(parts) == 1:  #  `path` is a sole file
            tree[parts[0]] = leaf
        else:  #  `path` still contains directories
            head, tail = parts
            if head not in tree:
                tree[head] = {}
            subtree = tree[head]
            assert not isinstance(subtree, str)  # => is PathTree
            _helper(tail, leaf, subtree)

    tree: PathTree = {}
    for path in paths:
        path = path.replace('\\', '/')
        _helper(path, path, tree)
    if len(tree) > 1:  # multiple root nodes
        tree = {name_of_root: tree}
    return tree


@dataclass
class PathRow:
    """Stores the data of a single row of the data frame created by
    :func:`build_frame_from_tree`.

    """

    path: str
    name: str
    id: int
    parent: int
    type: str


def build_frame_from_tree(tree: PathTree) -> List[PathRow]:
    """Creates a data frame from the given file hierarchy.

    The returned data frame has the following structure:

                   path         name    id  parent   type
        0        <root>       <root>     0      -1  vroot
        1     README.md    README.md     1       0   file
        2    .gitignore   .gitignore     2       0   file
        3           src          src     3       0    dir
        4   src/include      include     4       3    dir
        ...               ...          ...     ...    ...

    where `type` can take one of the following values:

        - `file`:  Indicates that the element is a file.
        - `dir`:   Indicates that the element is a directory.
        - `vroot`: Indicates that the root directory is a virtual root
                   node. There can only be one root directory and,
                   hence, only one element of type `vroot`. `vroot` is
                   not prepended to the path of the file/directory
                   elements.

    Only the root node has a negative parent id.

    Parameters:
    tree -- the file hierarchy to build up the data frame from

    """

    def _helper(
        tree: PathTree,
        parent_id: int,
        parent_path: str,
        next_id: int,
        frame: List[PathRow],
    ) -> int:
        """Helper function which builds up the data frame recursively.

        Note:
        The return value of this function is used to track the `id` of
        the node to be inserted next into `frame` during the recursive
        descent.

        Parameters:
        tree        -- the file hierarchy to build up the frame from
        parent_id   -- the id of the parent node
        parent_path -- the path of the parent node
        next_id     -- the id of the node to be inserted next into
                       `frame`
        frame       -- the resulting data frame

        """
        for key, val in tree.items():
            if isinstance(val, str):
                frame.append(PathRow(val, key, next_id, parent_id, 'file'))
                next_id = next_id + 1
            else:
                path = key
                if parent_id >= 0:  # not root node
                    path = parent_path + '/' + key
                frame.append(PathRow(path, key, next_id, parent_id, 'dir'))
                next_id = _helper(val, next_id, path, next_id + 1, frame)
        return next_id

    if len(tree) == 0:
        raise ValueError('empty tree')
    if len(tree) > 1:
        raise ValueError('multiple root nodes')
    root = next(iter(tree.keys()))

    frame: List[PathRow] = []
    _helper(tree, -1, '', 0, frame)
    if any(  # has artificial root node
        not row.path.startswith(root) for row in frame if row.type == 'file'
    ):
        prefix_len = len(root) + 1  # root + '/'
        for row in frame:
            if row.type == 'dir':
                if row.parent >= 0:  # not the artificial root node
                    # Remove `root` prefix from `path`.
                    row.path = row.path[prefix_len:]
                else:
                    # Change type of root to `vroot`.
                    row.type = 'vroot'
    return frame
