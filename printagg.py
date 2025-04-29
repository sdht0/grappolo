import sys

data = {}
with open(sys.argv[1]) as f:
    for node, line in enumerate(f):
        comm = int(line.strip())
        data.setdefault(comm, list()).append(node)

comms = []
for comm,nodes in data.items():
    comms.append([min(nodes), len(nodes), sum(nodes), comm, list(sorted(nodes))[:5]])

comms_10 = sorted(comms, key=lambda x: (-x[1], x[0]))[:10]
for c in comms_10:
    print(";".join([str(i) for i in c]))
