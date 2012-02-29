#include "dialign.h"
#include "config.h"
#include "Common/Options.h"
#include "ConstString.h"
#include "ContigNode.h"
#include "ContigPath.h"
#include "Dictionary.h"
#include "FastaReader.h"
#include "IOUtil.h"
#include "StringUtil.h"
#include "Uncompress.h"
#include "alignGlobal.h"
#include "Graph/ConstrainedSearch.h"
#include "Graph/ContigGraph.h"
#include "Graph/ContigGraphAlgorithms.h"
#include "Graph/GraphIO.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace std;

#define PROGRAM "PathConsensus"

static const char VERSION_MESSAGE[] =
PROGRAM " (" PACKAGE_NAME ") " VERSION "\n"
"Written by Shaun Jackman and Rong She.\n"
"\n"
"Copyright 2012 Canada's Michael Smith Genome Science Centre\n";

static const char USAGE_MESSAGE[] =
"Usage: " PROGRAM " [OPTION]... FASTA ADJ PATH\n"
"Align sequences of ambiguous paths and output a consensus\n"
"sequence.\n"
"  FASTA  contigs in FASTA format\n"
"  ADJ    contig adjacency graph\n"
"  PATH   paths of these contigs\n"
"\n"
" Options:\n"
"  -k, --kmer=N          k-mer size\n"
"  -d, --dist-error=N    acceptable error of a distance estimate\n"
"                        default: 6 bp\n"
"  -o, --out=FILE        output contig paths to FILE\n"
"  -s, --consensus=FILE  output consensus sequences to FILE\n"
"  -g, --graph=FILE      output the contig adjacency graph to FILE\n"
"  -a, --branches=N      maximum number of sequences to align\n"
"                        default: 4\n"
"  -p, --identity=REAL   minimum identity, default: 0.9\n"
"  -v, --verbose         display verbose output\n"
"      --help            display this help and exit\n"
"      --version         output version information and exit\n"
"\n"
" DIALIGN-TX options:\n"
"  -D, --dialign-d=N     dialign debug level, default: 0\n"
"  -M, --dialign-m=FILE  score matrix, default: dna_matrix.scr\n"
"  -P, --dialign-p=FILE  diagonal length probability distribution\n"
"                        default: dna_diag_prob_100_exp_550000\n"
"\n"
"Report bugs to <" PACKAGE_BUGREPORT ">.\n";

namespace opt {
	unsigned k; // used by ContigProperties
	static string out;
	static string consensusPath;
	static string graphPath;
	static float identity = 0.9;
	static unsigned numBranches = 4;
	static int dialign_debug;
	static string dialign_score;
	static string dialign_prob;

	/** Output format. */
	int format; // used by ContigProperties

	/** The the number of bases to continue the constrained search of
	 * the graph beyond the size of the ambiguous gap in the path.
	 */
	static unsigned distanceError = 6;
}

static const char shortopts[] = "d:k:o:s:g:a:p:vD:M:P:";

enum { OPT_HELP = 1, OPT_VERSION };

static const struct option longopts[] = {
	{ "kmer",        required_argument, NULL, 'k' },
	{ "dist-error",  required_argument, NULL, 'd' },
	{ "out",         required_argument, NULL, 'o' },
	{ "consensus",   required_argument, NULL, 's' },
	{ "graph",       required_argument, NULL, 'g' },
	{ "branches",    required_argument, NULL, 'a' },
	{ "identity",    required_argument, NULL, 'p' },
	{ "verbose",     no_argument,       NULL, 'v' },
	{ "help",        no_argument,       NULL, OPT_HELP },
	{ "version",     no_argument,       NULL, OPT_VERSION },
	{ "dialign-d",   required_argument, NULL, 'D' },
	{ "dialign-m",   required_argument, NULL, 'M' },
	{ "dialign-p",   required_argument, NULL, 'P' },
	{ NULL, 0, NULL, 0 }
};

static struct {
	unsigned numAmbPaths;
	unsigned numMerged;
	unsigned numNoSolutions;
	unsigned numTooManySolutions;
	unsigned tooComplex;
	unsigned notMerged;
} stats;

struct AmbPathConstraint {
	ContigNode	source;
	ContigNode	dest;
	int		dist;

	AmbPathConstraint(const ContigNode& src, const ContigNode& dst,
			int d)
		: source(src), dest(dst), dist(d) {}

	bool operator ==(const AmbPathConstraint& a) const
	{
		return source == a.source && dest == a.dest
			&& dist == a.dist;
	}

	bool operator <(const AmbPathConstraint& a) const
	{
		return source < a.source
			|| (source == a.source && dest < a.dest)
			|| (source == a.source && dest == a.dest
				&& dist < a.dist);
	}
};

typedef ContigPath Path;
typedef vector<Path> ContigPaths;
typedef map<AmbPathConstraint, ContigPath> AmbPath2Contig;

typedef vector<const_string> Contigs;
static Contigs g_contigs;
AmbPath2Contig g_ambpath_contig;

/** Return the sequence of the specified contig node. The sequence
 * may be ambiguous or reverse complemented.
 */
static const Sequence getSequence(ContigNode id)
{
	if (id.ambiguous()) {
		string s(id.ambiguousSequence());
		if (s.length() < opt::k)
			transform(s.begin(), s.end(), s.begin(), ::tolower);
		return string(opt::k - 1, 'N') + s;
	} else {
		string seq(g_contigs[id.id()]);
		return id.sense() ? reverseComplement(seq) : seq;
	}
}

/** Read contig paths from the specified file.
 * @param ids [out] the string ID of the paths
 * @param isAmb [out] whether the path contains a gap
 */
static ContigPaths readPath(const string& inPath,
	vector<string>& ids, vector<bool>& isAmb)
{
	assert(ids.empty()); //this seed is contigID of the path
	assert(isAmb.empty());
	assert(g_ambpath_contig.empty());
	ifstream fin(inPath.c_str());
	if (opt::verbose > 0)
		cerr << "Reading `" << inPath << "'..." << endl;
	if (inPath != "-")
		assert_good(fin, inPath);
	istream& in = inPath == "-" ? cin : fin;

	ContigPaths paths;
	string id;
	Path path;
	Path::iterator prev, next;
	while (in >> id >> path) {
		paths.push_back(path);
		ids.push_back(id);

		if (path.size() <= 2) {
			isAmb.push_back(false);
			continue;
		}

		/* contig with Ns must have prev and next */
		bool cur_is_amb = false;
		prev = path.begin();
		next = path.begin();
		Path::iterator it = path.begin();
		it++; next++; next++;
		for (; next != path.end(); it++, prev++, next++) {
			if (it->ambiguous()) {
				cur_is_amb = true;
				/* add the entry to g_ambpath_contig
				(value 0: unprocessed/no proper paths) */
				g_ambpath_contig.insert(AmbPath2Contig::value_type(
					AmbPathConstraint(*prev, *next, -it->id()),
					ContigPath()));
			}
		}
		isAmb.push_back(cur_is_amb);
	}
	assert(in.eof());
	return paths;
}

/** Mark every contig in path as seen. */
static void markSeen(vector<bool>& seen, const ContigPath& path,
		bool flag)
{
	for (Path::const_iterator it = path.begin();
			it != path.end(); ++it)
		if (!it->ambiguous() && it->id() < seen.size())
			seen[it->id()] = flag;
}

/** Mark every contig in paths as seen. */
static void markSeen(vector<bool>& seen, const vector<Path>& paths,
		bool flag)
{
	for (vector<Path>::const_iterator it = paths.begin();
			it != paths.end(); ++it)
		markSeen(seen, *it, flag);
}

struct NewVertex {
	Graph::vertex_descriptor t, u, v;
	Graph::vertex_property_type vp;
	NewVertex(
			Graph::vertex_descriptor t,
			Graph::vertex_descriptor u,
			Graph::vertex_descriptor v,
			const Graph::vertex_property_type& vp)
		: t(t), u(u), v(v), vp(vp) { }
};
typedef vector<NewVertex> NewVertices;

/** The new vertices that will be added to the graph. */
static NewVertices g_newVertices;

/** Output a new contig. */
static ContigID outputNewContig(
	const vector<Path>& solutions,
	size_t longestPrefix, size_t longestSuffix,
	const Sequence& seq, const unsigned coverage,
	ofstream& out)
{
	assert(!solutions.empty());
	assert(longestPrefix > 0);
	assert(longestSuffix > 0);
	ContigID id(ContigID::create());

	// Record the newly-created contig to be added to the graph later.
	g_newVertices.push_back(NewVertex(
				*(solutions[0].begin() + longestPrefix - 1),
				ContigNode(id, false),
				*(solutions[0].rbegin() + longestSuffix - 1),
				ContigProperties(seq.length(), coverage)));

	out << '>' << id.str() << ' ' << seq.length()
		<< ' ' << coverage << ' ';
	for (vector<Path>::const_iterator it = solutions.begin();
			it != solutions.end(); it++) {
		if (it != solutions.begin())
			out << ';';
		const ContigPath& path = *it;
		ContigPath::const_iterator
			first = path.begin() + longestPrefix,
			last = path.end() - longestSuffix;
		assert(first <= last);
		if (first < last) {
			copy(first, last - 1,
					ostream_iterator<ContigNode>(out, ","));
			out << *(last - 1);
		} else
			out << '*';
	}
	out << '\n' << seq << '\n';
	return id;
}

/** Return a consensus sequence of a and b.
 * @return an empty string if a consensus could not be found
 */
static string createConsensus(const Sequence& a, const Sequence& b)
{
	assert(a.length() == b.length());
	if (a == b)
		return a;
	string s;
	s.reserve(a.length());
	for (string::const_iterator ita = a.begin(), itb = b.begin();
			ita != a.end(); ++ita, ++itb) {
		bool mask = islower(*ita) || islower(*itb);
		char ca = toupper(*ita), cb = toupper(*itb);
		char c = ca == cb ? ca
			: ca == 'N' ? cb
			: cb == 'N' ? ca
			: 'x';
		if (c == 'x')
			return string("");
		s += mask ? tolower(c) : c;
	}
	return s;
}

/** Merge the specified two contigs, default overlap is k-1,
 * generate a consensus sequence of the overlapping region. The result
 * is stored in the first argument.
 */
static void mergeContigs(unsigned overlap, Sequence& seq,
		const Sequence& s, const ContigNode& node, const Path& path)
{
	assert(s.length() > overlap);
	Sequence ao;
	Sequence bo(s, 0, overlap);
	Sequence o;
	do {
		assert(seq.length() > overlap);
		ao = seq.substr(seq.length() - overlap);
		o = createConsensus(ao, bo);
	} while (o.empty() && chomp(seq, 'n'));
	if (o.empty()) {
		cerr << "warning: the head of `" << node << "' "
			"does not match the tail of the previous contig\n"
			<< ao << '\n' << bo << '\n' << path << endl;
		seq += 'n';
		seq += s;
	} else {
		seq.resize(seq.length() - overlap);
		seq += o;
		seq += Sequence(s, overlap);
	}
}

static Sequence mergePath(const Graph&g, const Path& path)
{
	Sequence seq;
	Path::const_iterator prev_it;
	for (Path::const_iterator it = path.begin();
			it != path.end(); ++it) {
		if (seq.empty()) {
			seq = getSequence(*it);
		} else {
			int d = get(edge_bundle, g, *(it-1), *it).distance;
			assert(d < 0);
			unsigned overlap = -d;
			mergeContigs(overlap, seq, getSequence(*it), *it, path);
		}
		prev_it = it;
	}
	return seq;
}

/** Calculate the ContigProperties of a path. */
static ContigProperties calculatePathProperties(const Graph& g,
		const ContigPath& path)
{
	return addProp(g, path.begin(), path.end());
}

/* Resolve ambiguous region using pairwise alignment
 * (Needleman-Wunsch) ('solutions' contain exactly two paths, from a
 * source contig to a dest contig)
 */
static ContigPath alignPair(const Graph& g,
		const ContigPaths& solutions, ofstream& out)
{
	assert(solutions.size() == 2);
	assert(solutions[0].size() > 1);
	assert(solutions[1].size() > 1);
	assert(solutions[0].front() == solutions[1].front());
	assert(solutions[0].back() == solutions[1].back());
	ContigPath fstSol(solutions[0].begin()+1, solutions[0].end()-1);
	ContigPath sndSol(solutions[1].begin()+1, solutions[1].end()-1);

	if (fstSol.empty() || sndSol.empty()) {
		// This entire sequence may be deleted.
		const ContigPath& sol(fstSol.empty() ? sndSol : fstSol);
		assert(!sol.empty());
		Sequence consensus(mergePath(g, sol));
		assert(consensus.size() > opt::k - 1);
		string::iterator first = consensus.begin() + opt::k - 1;
		transform(first, consensus.end(), first, ::tolower);

		unsigned match = opt::k - 1;
		float identity = (float)match / consensus.size();
		if (opt::verbose > 2)
			cerr << consensus << '\n';
		if (opt::verbose > 1)
			cerr << identity
				<< (identity < opt::identity ? " (too low)\n" : "\n");
		if (identity < opt::identity)
			return ContigPath();

		unsigned coverage = calculatePathProperties(g, sol).coverage;
		ContigID id = outputNewContig(
				solutions, 1, 1, consensus, coverage, out);
		ContigPath path;
		path.push_back(solutions.front().front());
		path.push_back(ContigNode(id, false));
		path.push_back(solutions.front().back());
		return path;
	}

	Sequence fstPathContig(mergePath(g, fstSol));
	Sequence sndPathContig(mergePath(g, sndSol));
	if (fstPathContig == sndPathContig) {
		// These two paths have identical sequence.
		if (fstSol.size() == sndSol.size()) {
			// A perfect match must be caused by palindrome.
			typedef ContigPath::const_iterator It;
			pair<It, It> it = mismatch(
					fstSol.begin(), fstSol.end(), sndSol.begin());
			assert(it.first != fstSol.end());
			assert(it.second != sndSol.end());
			assert(*it.first == ~*it.second);
			assert(equal(it.first+1, It(fstSol.end()), it.second+1));
			if (opt::verbose > 1)
				cerr << "Palindrome: " << ContigID(*it.first) << '\n';
			return solutions[0];
		} else {
			// The paths are different lengths.
			cerr << PROGRAM ": warning: "
				"Two paths have identical sequence, which may be "
				"caused by a transitive edge in the overlap graph.\n"
				<< '\t' << fstSol << '\n'
				<< '\t' << sndSol << '\n';
			return solutions[fstSol.size() > sndSol.size() ? 0 : 1];
		}
	}

	unsigned minLength = min(
			fstPathContig.length(), sndPathContig.length());
	unsigned maxLength = max(
			fstPathContig.length(), sndPathContig.length());
	float lengthRatio = (float)minLength / maxLength;
	if (lengthRatio < opt::identity) {
		if (opt::verbose > 1)
			cerr << minLength << '\t' << maxLength
				<< '\t' << lengthRatio << "\t(different length)\n";
		return ContigPath();
	}

	NWAlignment align;
	unsigned match = alignGlobal(fstPathContig, sndPathContig,
		   	align);
	float identity = (float)match / align.size();
	if (opt::verbose > 2)
		cerr << align;
	if (opt::verbose > 1)
		cerr << identity
			<< (identity < opt::identity ? " (too low)\n" : "\n");
	if (identity < opt::identity)
		return ContigPath();

	unsigned coverage = calculatePathProperties(g, fstSol).coverage
		+ calculatePathProperties(g, sndSol).coverage;
	ContigID id = outputNewContig(solutions, 1, 1,
			align.consensus(), coverage, out);
	ContigPath path;
	path.push_back(solutions.front().front());
	path.push_back(ContigNode(id, false));
	path.push_back(solutions.front().back());
	return path;
}

/* Resolve ambiguous region using multiple alignment of all paths in
 * `solutions'.
 */
static ContigPath alignMulti(const Graph& g,
		const vector<Path>& solutions, ofstream& out)
{
	// Find the size of the smallest path.
	const Path& firstSol = solutions.front();
	size_t min_len = firstSol.size();
	for (vector<Path>::const_iterator it = solutions.begin() + 1;
			it != solutions.end(); ++it)
		min_len = min(min_len, it->size());

	// Find the longest prefix.
	Path vppath;
	size_t longestPrefix;
	bool commonPrefix = true;
	for (longestPrefix = 0;
			longestPrefix < min_len; longestPrefix++) {
		const ContigNode& common_path_node = firstSol[longestPrefix];
		for (vector<Path>::const_iterator solIter = solutions.begin();
				solIter != solutions.end(); ++solIter) {
			const ContigNode& pathnode = (*solIter)[longestPrefix];
			if (pathnode != common_path_node) {
				// Found the longest prefix.
				commonPrefix = false;
				break;
			}
		}
		if (!commonPrefix)
			break;
		vppath.push_back(common_path_node);
	}

	// Find the longest suffix.
	Path vspath;
	size_t longestSuffix;
	bool commonSuffix = true;
	for (longestSuffix = 0;	longestSuffix < min_len-longestPrefix;
			longestSuffix++) {
		const ContigNode& common_path_node
			= firstSol[firstSol.size()-longestSuffix-1];
		for (vector<Path>::const_iterator solIter = solutions.begin();
				solIter != solutions.end(); ++solIter) {
			const ContigNode& pathnode
				= (*solIter)[solIter->size()-longestSuffix-1];
			if (pathnode != common_path_node) {
				// Found the longest suffix.
				commonSuffix = false;
				break;
			}
		}
		if (!commonSuffix)
			break;
		vspath.push_back(common_path_node);
	}
	reverse(vspath.begin(), vspath.end());

	if (opt::verbose > 1 && vppath.size() + vspath.size() > 2)
		cerr << vppath << " * " << vspath << '\n';

	// Get sequence of ambiguous region in paths
	assert(longestPrefix > 0 && longestSuffix > 0);
	vector<Sequence> amb_seqs;
	unsigned coverage = 0;
	for (vector<Path>::const_iterator solIter = solutions.begin();
			solIter != solutions.end(); solIter++) {
		assert(longestPrefix + longestSuffix <= solIter->size());
		Path path(solIter->begin() + longestPrefix,
				solIter->end() - longestSuffix);
		if (!path.empty()) {
			amb_seqs.push_back(mergePath(g, path));
			coverage += calculatePathProperties(g, path).coverage;
		} else {
			// The prefix and suffix paths overlap by k-1 bp.
			Sequence s = getSequence(solutions[0][longestPrefix-1]);
			amb_seqs.push_back(s.substr(s.length() - opt::k + 1));
		}
	}

	vector<unsigned> lengths(amb_seqs.size());
	transform(amb_seqs.begin(), amb_seqs.end(), lengths.begin(),
			mem_fun_ref(&string::length));
	unsigned minLength = *min_element(lengths.begin(), lengths.end());
	unsigned maxLength = *max_element(lengths.begin(), lengths.end());
	float lengthRatio = (float)minLength / maxLength;
	if (lengthRatio < opt::identity) {
		if (opt::verbose > 1)
			cerr << minLength << '\t' << maxLength
				<< '\t' << lengthRatio << "\t(different length)\n";
		return ContigPath();
	}

	string alignment;
	unsigned matches;
	Sequence consensus = dialign(amb_seqs, alignment, matches);
	if (opt::verbose > 2)
	   	cerr << alignment << consensus << '\n';
	float identity = (float)matches / consensus.size();
	if (opt::verbose > 1)
		cerr << identity
			<< (identity < opt::identity ? " (too low)\n" : "\n");
	if (identity < opt::identity)
		return ContigPath();

	if (identity == 1) {
		// A perfect match must be caused by two palindromes.
		ContigID palindrome0 = solutions[0][longestPrefix];
		ContigID palindrome1 = solutions[0].rbegin()[longestSuffix];
		if (opt::verbose > 1)
			cerr << "Palindrome: " << palindrome0 << '\n'
				<< "Palindrome: " << palindrome1 << '\n';
#ifndef NDEBUG
		string s0 = getSequence(ContigNode(palindrome0, false));
		string s1 = getSequence(ContigNode(palindrome1, false));
		assert(s0 == reverseComplement(s0));
		assert(s1 == reverseComplement(s1));
		for (vector<Path>::const_iterator it = solutions.begin();
				it != solutions.end(); ++it) {
			const ContigPath& path = *it;
			assert(ContigID(path[longestPrefix]) == palindrome0);
			assert(ContigID(path.rbegin()[longestSuffix])
					== palindrome1);
			assert(path.size() == solutions[0].size());
		}
#endif
		return solutions[0];
	}

	ContigID id = outputNewContig(solutions,
		longestPrefix, longestSuffix, consensus, coverage, out);
	ContigPath path(vppath);
	path.push_back(ContigNode(id, false));
	path.insert(path.end(), vspath.begin(), vspath.end());
	return path;
}

/** Align the sequences of the specified paths.
 * @return the consensus sequence
 */
static ContigPath align(const Graph& g, const vector<Path>& sequences,
		ofstream& out)
{
	assert(sequences.size() > 1);
	return sequences.size() == 2
		? alignPair(g, sequences, out)
		: alignMulti(g, sequences, out);
}

/** Return the consensus sequence of the specified gap. */
static ContigPath fillGap(const Graph& g,
		const AmbPathConstraint& apConstraint,
		vector<bool>& seen,
		ofstream& outFasta)
{
	if (opt::verbose > 1)
		cerr << "\n* " << apConstraint.source << ' '
			<< apConstraint.dist << "N "
			<< apConstraint.dest << '\n';

	Constraints constraints;
	constraints.push_back(Constraint(apConstraint.dest,
				apConstraint.dist + opt::distanceError));

	ContigPaths solutions;
	unsigned numVisited = 0;
	constrainedSearch(g, apConstraint.source,
			constraints, solutions, numVisited);
	bool tooComplex = numVisited >= opt::maxCost;

	for (ContigPaths::iterator solIt = solutions.begin();
			solIt != solutions.end(); solIt++)
		solIt->insert(solIt->begin(), apConstraint.source);

	ContigPath consensus;
	bool tooManySolutions = solutions.size() > opt::numBranches;
	if (tooComplex) {
		stats.tooComplex++;
		if (opt::verbose > 1)
			cerr << solutions.size() << " paths (too complex)\n";
	} else if (tooManySolutions) {
		stats.numTooManySolutions++;
		if (opt::verbose > 1)
			cerr << solutions.size() << " paths (too many)\n";
	} else if (solutions.empty()) {
		stats.numNoSolutions++;
		if (opt::verbose > 1)
			cerr << "no paths\n";
	} else if (solutions.size() == 1) {
		if (opt::verbose > 1)
			cerr << "1 path\n" << solutions.front() << '\n';
		stats.numMerged++;
	} else {
		assert(solutions.size() > 1);
		if (opt::verbose > 2)
			copy(solutions.begin(), solutions.end(),
					ostream_iterator<ContigPath>(cerr, "\n"));
		else if (opt::verbose > 1)
			cerr << solutions.size() << " paths\n";
		consensus = align(g, solutions, outFasta);
		if (!consensus.empty()) {
			stats.numMerged++;
			// Mark contigs that are used in a consensus.
			markSeen(seen, solutions, true);
			if (opt::verbose > 1)
				cerr << consensus << '\n';
		} else
			stats.notMerged++;
	}
	return consensus;
}

int main(int argc, char** argv)
{
	string commandLine;
	{
		ostringstream ss;
		char** last = argv + argc - 1;
		copy(argv, last, ostream_iterator<const char *>(ss, " "));
		ss << *last;
		commandLine = ss.str();
	}

	bool die = false;
	for (int c; (c = getopt_long(argc, argv,
			shortopts, longopts, NULL)) != -1;) {
		istringstream arg(optarg != NULL ? optarg : "");
		switch (c) {
		case '?': die = true; break;
		case 'd': arg >> opt::distanceError; break;
		case 'k': arg >> opt::k; break;
		case 'o': arg >> opt::out; break;
		case 'p': arg >> opt::identity; break;
		case 'a': arg >> opt::numBranches; break;
		case 's': arg >> opt::consensusPath; break;
		case 'g': arg >> opt::graphPath; break;
		case 'D': arg >> opt::dialign_debug; break;
		case 'M': arg >> opt::dialign_score; break;
		case 'P': arg >> opt::dialign_prob; break;
		case 'v': opt::verbose++; break;
		case OPT_HELP:
			cout << USAGE_MESSAGE;
			exit(EXIT_SUCCESS);
		case OPT_VERSION:
			cout << VERSION_MESSAGE;
			exit(EXIT_SUCCESS);
		}
		if (optarg != NULL && !arg.eof()) {
			cerr << PROGRAM ": invalid option: `-"
				<< (char)c << optarg << "'\n";
			exit(EXIT_FAILURE);
		}
	}

	if (opt::k <= 0) {
		cerr << PROGRAM ": missing -k,--kmer option\n";
		die = true;
	}

	if (opt::out.empty()) {
		cerr << PROGRAM ": " << "missing -o,--out option\n";
		die = true;
	}

	if (opt::consensusPath.empty()) {
		cerr << PROGRAM ": " << "missing -s,--consensus option\n";
		die = true;
	}

	if (argc - optind < 3) {
		cerr << PROGRAM ": missing arguments\n";
		die = true;
	}

	if (die) {
		cerr << "Try `" << PROGRAM
			<< " --help' for more information.\n";
		exit(EXIT_FAILURE);
	}

	const char *contigFile = argv[optind++];
	string adjFile(argv[optind++]);
	string allPaths(argv[optind++]);

	// Read the contig overlap graph.
	if (opt::verbose > 0)
		cerr << "Reading `" << adjFile << "'..." << endl;
	ifstream fin(adjFile.c_str());
	assert_good(fin, adjFile);
	Graph g;
	fin >> g;
	assert(fin.eof());

	// Read contigs
	Contigs& contigs = g_contigs;
	{
		if (opt::verbose > 0)
			cerr << "Reading `" << contigFile << "'..." << endl;
		FastaReader in(contigFile, FastaReader::NO_FOLD_CASE);
		for (FastaRecord rec; in >> rec;) {
			ContigID id(rec.id);
			assert(contigs.size() == id);
			contigs.push_back(rec.seq);
		}
		assert(in.eof());
		assert(!contigs.empty());
		opt::colourSpace = isdigit(contigs[0][0]);
	}
	ContigID::lock();

	vector<string> pathIDs;
	vector<bool> isAmbPath;
	ContigPaths paths = readPath(allPaths, pathIDs, isAmbPath);
	stats.numAmbPaths = g_ambpath_contig.size();
	if (opt::verbose > 0)
		cerr << "Read " << paths.size() << " paths\n";

	// Start numbering new contigs from the last
	if (!pathIDs.empty())
		ContigID::setNextContigID(pathIDs.back());

	// Prepare output fasta file
	ofstream fa(opt::consensusPath.c_str());
	assert_good(fa, opt::consensusPath);

	init_parameters();
	set_parameters_dna();
	para->DEBUG = opt::dialign_debug;
	para->SCR_MATRIX_FILE_NAME = (char*)opt::dialign_score.c_str();
	para->DIAG_PROB_FILE_NAME = (char*)opt::dialign_prob.c_str();
	initDialign();

	// Contigs that were seen in a consensus.
	vector<bool> seen(contigs.size());

	// resolve ambiguous paths recorded in g_ambpath_contig
	for (AmbPath2Contig::iterator ambIt = g_ambpath_contig.begin();
			ambIt != g_ambpath_contig.end(); ambIt++)
		ambIt->second = fillGap(g, ambIt->first, seen, fa);
	assert_good(fa, opt::consensusPath);
	fa.close();
	if (opt::verbose > 1)
		cerr << '\n';

	// Unmark contigs that are used in a path.
	for (AmbPath2Contig::iterator it = g_ambpath_contig.begin();
			it != g_ambpath_contig.end(); it++)
		markSeen(seen, it->second, false);
	markSeen(seen, paths, false);

	ofstream out(opt::out.c_str());
	assert_good(out, opt::out);

	// Output those contigs that were not seen in ambiguous path.
	for (unsigned id = 0; id < contigs.size(); ++id)
		if (seen[id])
			out << ContigID(id) << '\n';

	for (ContigPaths::const_iterator path = paths.begin();
			path != paths.end(); ++path) {
		unsigned i = path - paths.begin();
		if (!isAmbPath[i]) {
			out << pathIDs[i] << '\t' << *path << '\n';
			continue;
		}

		assert(path->size() > 2);
		Path cur_path;
		cur_path.push_back(path->front());
		for (Path::const_iterator prev = path->begin(),
				it = path->begin() + 1, next = path->begin() + 2;
				it != path->end(); ++prev, ++it, ++next) {
			if (!it->ambiguous()) {
				cur_path.push_back(*it);
				continue;
			}

			//replace Ns by resolved new contig
			assert(next != path->end());
			AmbPath2Contig::iterator ambIt = g_ambpath_contig.find(
				AmbPathConstraint(*prev, *next, -it->id()));
			assert(ambIt != g_ambpath_contig.end());
			const ContigPath& solution = ambIt->second;
			if (!solution.empty()) {
				assert(solution.size() > 1);
				cur_path.insert(cur_path.end(),
						solution.begin() + 1, solution.end() - 1);
			} else
				cur_path.push_back(*it);
		}
		out << pathIDs[i] << '\t' << cur_path << '\n';
	}
	assert_good(out, opt::out);
	out.close();

	free_prob_dist(pdist);
	free(para);

	cerr <<
		"Ambiguous paths: " << stats.numAmbPaths << "\n"
		"Merged:          " << stats.numMerged << "\n"
		"No paths:        " << stats.numNoSolutions << "\n"
		"Too many paths:  " << stats.numTooManySolutions << "\n"
		"Too complex:     " << stats.tooComplex << "\n"
		"Dissimilar:      " << stats.notMerged << "\n";

	if (!opt::graphPath.empty()) {
		ofstream fout(opt::graphPath.c_str());
		assert_good(fout, opt::graphPath);

		// Add the newly-created consensus contigs to the graph.
		for (NewVertices::const_iterator it = g_newVertices.begin();
				it != g_newVertices.end(); ++it) {
			Graph::vertex_descriptor u = add_vertex(it->vp, g);
			assert(u == it->u);
			add_edge(it->t, it->u, g);
			add_edge(it->u, it->v, g);
		}
		write_graph(fout, g, PROGRAM, commandLine);
		assert_good(fout, opt::graphPath);
	}

	return 0;
}
