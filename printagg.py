import sys

data = {}
nodes = {}

with open(sys.argv[2]) as f:
    for node, line in enumerate(f):
        parts = line.strip().split(',')
        csv_id = int(parts[0])
        internal_id = int(parts[1].split(':')[-1])
        nodes[internal_id] = csv_id

with open(sys.argv[1]) as f:
    for node, line in enumerate(f):
        comm = int(line.strip())
        r = data.setdefault(comm, (list(), list()))
        r[0].append(node)
        r[1].append(nodes[node])

comms = []
for comm,(internal_nodes, csv_nodes) in data.items():
    comms.append([min(csv_nodes), len(csv_nodes), sum(csv_nodes), comm, list(sorted(csv_nodes))[:5]])

comms_10 = sorted(comms, key=lambda x: (-x[1], x[0]))[:10]
for c in comms_10:
    print(";".join([str(i) for i in c]))
