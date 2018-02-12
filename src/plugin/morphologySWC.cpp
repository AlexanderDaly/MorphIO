
#include "morphologySWC.h"

#include "../detail/skipWhiteSpace.h"

#include <cassert>
#include <fstream>
#include <list>
#include <sstream>

/*
#include <lunchbox/debug.h>
#include <lunchbox/log.h>
#include <lunchbox/pluginRegisterer.h>
*/

//TODO: compile
#include <iostream>

namespace minimorph
{
namespace plugin
{
namespace
{
// It's not clear if -1 is the only way of identifying a root section.
const int SWC_UNDEFINED_PARENT = -1;

enum SWCSectionType
{
    SWC_SECTION_UNDEFINED = 0,
    SWC_SECTION_SOMA = 1,
    SWC_SECTION_AXON = 2,
    SWC_SECTION_DENDRITE = 3,
    SWC_SECTION_APICAL_DENDRITE = 4,
    SWC_SECTION_FORK_POINT = 5,
    SWC_SECTION_END_POINT = 6,
    SWC_SECTION_CUSTOM = 7
};

template <typename TPoint> bool setPoint(const char* line, SWCSectionType &type, TPoint &point, float &radius, int &parent);

template <> bool setPoint(const char* line, SWCSectionType &type, Point &point, float &radius, int &parent){

    return sscanf(line, "%20d%20f%20f%20f%20f%20d", (int*)&type, &point[0],
               &point[1], &point[2], &radius, &parent) == 6;
}

struct Sample
{
    Sample()
        : valid(false)
        , type(SWC_SECTION_UNDEFINED)
        , parent(-1)
        , nextID(-1)
        , siblingID(-1)
        , parentSection(-1)
    {
    }

    explicit Sample(const char* line)
        : nextID(-1)
        , siblingID(-1)
        , parentSection(-1)
    {
        float radius;
        valid = setPoint(line, type, point, radius, parent);
        diameter = radius * 2; // The point array stores diameters.

        if (type >= SWC_SECTION_CUSTOM)
            valid = false; // Unknown section type, custom samples are also
                           // Regarded as unknown.
    }

    float diameter;
    bool valid;
    Point point; // x, y, z and diameter
    SWCSectionType type;
    int parent;
    int nextID;
    int siblingID;
    int parentSection; // Only meaningful for the first sample of each
                       // section
};
typedef std::vector<Sample> Samples;

//lunchbox::PluginRegisterer<MorphologySWC> registerer;

void _correctSampleType(Sample& sample, const Samples& samples)
{
    SWCSectionType type = sample.type;
    // Fork and end point sample types don't make much sense for Brion.
    // The sample type will be corrected to be that of the parent section.
    // If the parent is the soma or doesn't exist, the final type will be
    // undefined. If the parent is also a fork point, the traversal will
    // continue.
    int parentID = sample.parent;
    while (type == SWC_SECTION_FORK_POINT || type == SWC_SECTION_END_POINT)
    {
        if (parentID == SWC_UNDEFINED_PARENT)
        {
            sample.type = SWC_SECTION_UNDEFINED;
            return;
        }

        type = samples[parentID].type;
        parentID = samples[parentID].parent;

        if (type == SWC_SECTION_SOMA || type == SWC_SECTION_UNDEFINED)
        {
            sample.type = SWC_SECTION_UNDEFINED;
            return;
        }
    }

    sample.type = type;
}
}

struct MorphologySWC::RawSWCInfo
{
    RawSWCInfo()
        : totalValidSamples(0)
        , numSections(0)
    {
    }

    std::string filename;

    // This is the raw sample array. It will have gaps for those ids that
    // are missing in the input file.
    Samples samples;
    size_t totalValidSamples;

    // Depending on the input file there might be one or more samples with
    // no parent. This structure is used to be able to do a depth first
    // traversal from the soma. The first root is the soma and only one
    // root sample is allowed to be of type soma.
    std::vector<size_t> roots;

    size_t numSections;
};

MorphologySWC::MorphologySWC(const MorphologyInitData& initData)
    : MorphologyPlugin(initData)
{
    load();
}

void MorphologySWC::load()
{
    // Parsing SWC according to this specification:
    // http://www.neuronland.org/NLMorphologyConverter/MorphologyFormats/SWC/Spec.html
    // Sample numbers may not be contiguous and parent samples can appear
    // later than child ones. Both things shouldn't happen, but the "spec"
    // doesn't enforce it, only gives recommendations.
    // This code takes that possibility into account.

    RawSWCInfo info;
    info.filename = _data.uri;

    _readSamples(info);
    _buildSampleTree(info);
    _buildStructure(info);
    _data.family = FAMILY_NEURON;
    _data.version = MORPHOLOGY_VERSION_SWC_1;
}

bool MorphologySWC::handles(const MorphologyInitData& initData)
{
    //TODO: compile
    //const std::string& scheme = initData.getURI().getScheme();
    const std::string scheme;
    if (scheme != "file" && !scheme.empty())
        return false;

    //TODO: compile
    //const std::string path = initData.getURI().getPath();
    const std::string path;
    const size_t pos = path.find_last_of(".");
    if (pos == std::string::npos)
        return false;
    return path.substr(pos) == ".swc";
}

std::string MorphologySWC::getDescription()
{
    return "SWC morphologies:\n"
           "  [file://]/path/to/morphology.swc";
}

void MorphologySWC::_readSamples(RawSWCInfo& info)
{
    std::ifstream file(info.filename.c_str());
    if (file.fail())
        LBTHROW(minimorph::RawDataError("Error opening morphology file: " +
                                   info.filename));

    Samples& samples = info.samples;

    size_t lineNumber = 0;
    file >> detail::SkipWhiteSpace(lineNumber);
    std::string line;
    size_t totalSamples = 0;
    while (!std::getline(file, line).fail())
    {
        ++lineNumber;
        // Fix #4: Subsequent non-empty lines each represent a single neuron
        // sample point with seven data items.
        if (line[0] == '#' || line.empty())
            continue;

        char* data;
        const unsigned int id = strtol(line.data(), &data, 10);
        if (*data != ' ' && *data != '\t')
        {
            LBTHROW(minimorph::RawDataError(
                "Reading swc morphology file: " + info.filename +
                ", parse error at line " + std::to_string(lineNumber)));
        }
        samples.resize(std::max(samples.size(), size_t(id + 1)));
        if (samples[id].valid)
        {
            LBTHROW(minimorph::IDSequenceError(
                        "Reading swc morphology file: " + info.filename
                        + ", repeated sample id " + std::to_string(id) + " at line "
                        + std::to_string(lineNumber)));
        }
        else
        {
            samples[id] = Sample(data);
            ++totalSamples;
            if (!samples[id].valid)
            {
                LBTHROW(minimorph::IDSequenceError(
                    "Reading swc morphology file: " + info.filename +
                    ", parse error at line " + std::to_string(lineNumber)));
            }
        }

        file >> detail::SkipWhiteSpace(lineNumber);
    }
    info.totalValidSamples = totalSamples;
}

void MorphologySWC::_buildSampleTree(RawSWCInfo& info)
{
    // To connect the samples in a descending tree the easiest thing is to
    // start with the last sample (assumed to be and end-point as no other
    // can have it as parent without making a loop) and traverse the
    // morphology tree backwards until we find a sample with no
    // parent. Then, the next sample in the input vector which hasn't
    // still been processed is searched and the traversal continues from
    // there. If at any moment a sample which has already been visited is
    // hit, the loop goes back to search the next end point.
    std::vector<bool> visited;
    Samples& samples = info.samples;
    for (Sample& sample : samples)
        visited.push_back(!sample.valid);

    if (samples.empty())
        LBTHROW(minimorph::SomaError("Reading swc morphology file: " +
                                 info.filename + ", no soma section found"));

    size_t currentSample = samples.size() - 1;
    size_t currentEndPoint = samples.size() - 1;
    size_t samplesLeft = info.totalValidSamples;
    assert(!visited[currentSample]); // It's impossible that the last
                                     // sample array is not valid

    bool hasSoma = false;

    while (samplesLeft--)
    {
        Sample& sample = samples[currentSample];
        visited[currentSample] = true;

        _correctSampleType(sample, samples);

        // Moving to the parent if not visited yet, otherwise searching
        // for the next end point.
        const bool root = sample.parent == SWC_UNDEFINED_PARENT;

        if (!root)
        {
            if (sample.parent == int(currentSample))
            {
                LBTHROW(minimorph::IDSequenceError("Reading swc morphology file: " +
                                           info.filename +
                                           ", found a sample point to itself"));
            }
            Sample* parent = &samples[sample.parent];
            if (!parent->valid)
            {
                std::stringstream msg;
                msg << "Reading swc morphology file: " << info.filename
                    << ", broken tree (missing sample  " << sample.parent << ")"
                    << std::endl;
                LBTHROW(minimorph::MissingParentError(msg.str()));
            }

            if (parent->type == SWC_SECTION_SOMA)
            {
                // When the parent is the soma we have to handle it differently
                // as we don't want to split a soma ring where neurites
                // connect to arbitrary soma points in multiple sections.
                if (sample.type == SWC_SECTION_SOMA)
                {
                    if (parent->nextID != -1)
                    {
                        LBWARN
                            << "Reading swc morphology file: " << info.filename
                            << ", found bifurcation in soma section";
                        sample.siblingID = parent->nextID;
                    }
                    // Linking the parent to this sample.
                    parent->nextID = int(currentSample);
                }
                else
                {
                    info.roots.push_back(currentSample);
                    // Sections whose parent is the soma need their parent
                    // section to be assigned at this point.
                    sample.parentSection = 0;
                }
            }
            else
            {
                if (sample.type == SWC_SECTION_SOMA)
                {
                    LBTHROW(minimorph::SomaError(
                        "Reading swc morphology file: " + info.filename +
                        ", found soma sample with neurite parent"));
                }
                if (parent->nextID != -1)
                {
                    // The parent was already connected. Linking this sample
                    // to its sibling.
                    sample.siblingID = parent->nextID;
                    // This also means that a sequence of samples is now split
                    // in three different sections (a parent and two children).
                    info.numSections += 2;
                }
                // Linking the parent to this sample.
                parent->nextID = int(currentSample);
            }
        }
        else
        {
            ++info.numSections;
            if (sample.type == SWC_SECTION_SOMA)
            {
                // Only one soma section is permitted. If a previous
                // one is detected, then throw.
                if (info.roots.size() &&
                    samples[info.roots[0]].type == SWC_SECTION_SOMA)
                {
                    LBTHROW(minimorph::SomaError("Reading swc morphology file: " +
                                               info.filename +
                                               ", found two soma sections"));
                }
                info.roots.insert(info.roots.begin(), currentSample);
                hasSoma = true;
            }
            else
            {
                info.roots.push_back(currentSample);
                // Non soma root sections get their parent section
                // assigned to the soma at this point.
                sample.parentSection = 0;
                // If the sample type is fork or end point, we convert it
                // into undefined because we don't really know which is the
                // type of the section.
                if (sample.type == SWC_SECTION_FORK_POINT ||
                    sample.type == SWC_SECTION_END_POINT)
                {
                    sample.type = SWC_SECTION_UNDEFINED;
                }
            }
        }

        if (samplesLeft)
        {
            if (root || visited[sample.parent])
            {
                // This loop and the external loop guarantee that every
                // sample after currentEndPoint has been already visited (or
                // is invalid).
                while (visited[currentEndPoint])
                {
                    assert(currentEndPoint != 0);
                    --currentEndPoint;
                }
                currentSample = currentEndPoint;
            }
            else
                currentSample = sample.parent;
        }
    }
    if (!hasSoma)
        LBTHROW(minimorph::SomaError("Reading swc morphology file: " +
                                 info.filename + ", no soma section found"));
}

void MorphologySWC::_buildStructure(RawSWCInfo& info)
{
    std::list<size_t> sectionQueue(info.roots.begin(), info.roots.end());

    int section = 0;
    // All sections except the soma section and the first order sections
    // repeat the parent point in the point list. This is a "feature" to
    // stay compatible with the binary layout of the .h5 file format.
    // The size reservation is an upper estimate because it's not easy to
    // detect all first order sections (some may connect to the soma point
    // or ring and some may not).
    _properties.get<Property::Point>().reserve(info.totalValidSamples + info.numSections -
                    info.roots.size());
    _properties.get<Property::Section>().reserve(info.numSections);
    _properties.get<Property::SectionType>().reserve(info.numSections);
    Samples& samples = info.samples;

    Sample* sample = &samples[sectionQueue.front()];
    sectionQueue.pop_front();
    while (sample)
    {
        _properties.get<Property::Section>().push_back({int(_properties.get<Property::Point>().size()), sample->parentSection});
        _properties.get<Property::SectionType>().push_back(SectionType(sample->type));

        // Pushing first point of the section using the parent sample
        // if necessary
        if (sample->parent != SWC_UNDEFINED_PARENT)
        {
            const Sample* parent = &samples[sample->parent];

            // // Special case when parent is a single point soma:
            // // Parent is replaced by a new point at the intersection between the circle
            // // defined by the single point soma and the soma center to current point line
            // if (parent->type == SWC_SECTION_SOMA && parent->nextID == -1 && parent->parent == -1)
            // {
            //     const Vector4f& soma = parent->point;
            //     const Vector4f& current_point = sample->point;
            //     const Vector4f& radial_vector = current_point - soma;
            //     float distance = radial_vector.get_sub_vector<3, 0>().length();
            //     float soma_radius = soma[3]/ 2.;
            //     float extrapolation_factor = std::min((float)1., soma_radius / distance);
            //     Vector4f new_point = soma + extrapolation_factor * radial_vector;
            //     new_point[3] = current_point[3];
            //     if(!new_point.equals(current_point, 1e-6))
            //         _properties.get<Property::Point>().push_back(new_point);
            // } else {
            //     _properties.get<Property::Point>().push_back(parent->point);
            // }
        }

        // Iterate while we stay on the same section and push points
        // to the point vector.
        while (sample && (sample->siblingID == -1 || sample->type == SWC_SECTION_SOMA) &&
               // We also cut into sections when the sample type changes.
               // There are degenerated cases where this is absolutely
               // needed (e.g. a morphology with only one first order
               // section a point soma).
               sample->type == SWCSectionType(_properties.get<Property::SectionType>().back()))
        {
            _properties.get<Property::Point>().push_back(sample->point);
            _properties.get<Property::Diameter>().push_back(sample->diameter);

            if(sample->nextID != -1)
                sample = &samples[sample->nextID];
            // Special case of 3 point soma: sibling is part of the same "section"
            else if(sample->type == SWC_SECTION_SOMA && sample->siblingID != -1)
                sample = &samples[sample->siblingID];
            else
                sample = 0;
        }

        // Finished iterating a section
        if (sample)
        {
            // We reached a bifurcation or a section type change, pushing
            // the sibling (if any) to the sectionQeueue and continuing
            // traversing the current section
            assert(sample->siblingID != -1 ||
                   sample->type != SWCSectionType(_properties.get<Property::SectionType>().back()));

            // Assigning the parent section to the current sample, this
            // will be stored in the section array at the beginning of
            // the next iteration.
            sample->parentSection = section;

            // Pushing all siblings into the queue and unlinking them
            int siblingID = sample->siblingID;
            sample->siblingID = -1;
            while (siblingID != -1)
            {
                // We do push_front to continue on the same subtree and
                // do a depth-first traversal.
                sectionQueue.push_front(siblingID);
                Sample* sibling = &samples[siblingID];
                // Assigning the parent section to the sibling
                sibling->parentSection = section;
                siblingID = sibling->siblingID;
                sibling->siblingID = -1;
            }
        }
        else if (!sectionQueue.empty())
        {
            // Reached an end point. Starting the next section from
            // the sectionQueue if not empty
            sample = &samples[sectionQueue.front()];
            sectionQueue.pop_front();
        }
        ++section;
    }
}
}
}