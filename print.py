import sys

data = {}
with open(sys.argv[1]) as f:
    for node, line in enumerate(f):
        comm = int(line.strip())
        data.setdefault(comm, list()).append(node)

for comm, nodes in data.items():
    print(comm, list(sorted(nodes)))
