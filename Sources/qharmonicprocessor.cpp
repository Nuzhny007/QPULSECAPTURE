#include <QXmlStreamReader>
#include <QXmlStreamAttributes>
#include <QFile>
#include "qharmonicprocessor.h"

//----------------------------------------------------------------------------------------------------------
QHarmonicProcessor::QHarmonicProcessor(QObject *parent, quint16 length_of_data, quint16 length_of_buffer) :
    QObject(parent),
    datalength(length_of_data),
    bufferlength(length_of_buffer),
    curpos(0),
    SNRE(-5.0),
    HRfrequency(0.0),
    ch1_mean(0.0),
    ch2_mean(0.0),
    PCA_flag(false),
    m_channel(Green),
    m_zerocrossing(0),
    m_output(1.0),
    m_zerocrossingCounter(4),
    m_strobeValue(STROBE_FACTOR),
    m_accumulator(0.0),
    m_pos(0),
    m_leftThreshold(70),
    m_rightTreshold(80)
{
    // Memory allocation
    ptData_ch1 = new qreal[datalength];
    ptData_ch2 = new qreal[datalength];
    ptCNSignal = new qreal[datalength];
    ptTime = new qreal[datalength];
    ptX = new qreal[DIGITAL_FILTER_LENGTH];
    ptDataForFFT = new qreal[bufferlength];
    ptSpectrum = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * (bufferlength/2 + 1));
    ptAmplitudeSpectrum = new qreal[bufferlength/2 + 1];
    m_plan = fftw_plan_dft_r2c_1d(bufferlength, ptDataForFFT, ptSpectrum, FFTW_ESTIMATE);
    pt_Youtput = new qreal[datalength];
    pt_Xoutput = new qreal[DIGITAL_FILTER_LENGTH];
    pt_SlowPPG = new qreal[datalength];

    // Vectors initialization
    for (quint16 i = 0; i < datalength; i++)
    {
        ptData_ch1[i] = 0.0; // it should be equal to zero at start
        ptData_ch2[i] = 0.0; // it should be equal to zero at start
        ptTime[i] = 35.0; // just for ensure that at the begining there is not any "division by zero"
        ptCNSignal[i] = 0.0;
        pt_SlowPPG[i] = 0.0;
        if(i % 4)
        {
            pt_Youtput[i] = m_output;
        }
        else
        {
            pt_Youtput[i] = - m_output;
        }
    }

    // Memory allocation block for ALGLIB arrays
    PCA_RAW_RGB.setlength(bufferlength, 3); // 3 because RED, GREEN and BLUE colors represent 3 independent variables
    PCA_Variance.setlength(3);
    PCA_Basis.setlength(3, 3);
    PCA_Info = 0;
}

//----------------------------------------------------------------------------------------------------------

QHarmonicProcessor::~QHarmonicProcessor()
{
    fftw_destroy_plan(m_plan);
    delete[] ptData_ch1;
    delete[] ptData_ch2;
    delete[] ptCNSignal;
    delete[] ptTime;
    delete[] ptX;
    delete[] ptDataForFFT;
    fftw_free(ptSpectrum);
    delete[] ptAmplitudeSpectrum;
    delete[] pt_Youtput;
    delete[] pt_Xoutput;
    delete[] pt_SlowPPG;
}

//----------------------------------------------------------------------------------------------------------

void QHarmonicProcessor::WriteToDataRGB(unsigned long red, unsigned long green, unsigned long blue, unsigned long area, double time)
{
    quint16 position = loop_for_PCA(curpos);
    PCA_RAW_RGB(position, 0) = (qreal)red / area;
    PCA_RAW_RGB(position, 1) = (qreal)green / area;
    PCA_RAW_RGB(position, 2) = (qreal)blue / area;

    qreal ch1_temp = (qreal)(red - green) / area;
    qreal ch2_temp = (qreal)(red + green - 2 * blue) / area;

    ch1_mean += (ch1_temp - ptData_ch1[curpos]) / datalength;
    ch2_mean += (ch2_temp - ptData_ch2[curpos]) / datalength;

    ptData_ch1[curpos] = ch1_temp;
    ptData_ch2[curpos] = ch2_temp;

    ptTime[curpos] = time;
    emit ptTimeWasUpdated(ptTime, datalength);

    qreal ch1_sko = 0.0;
    qreal ch2_sko = 0.0;
    for (unsigned int i = 0; i < datalength; i++)
    {
        ch1_sko += (ptData_ch1[i] - ch1_mean)*(ptData_ch1[i] - ch1_mean);
        ch2_sko += (ptData_ch2[i] - ch2_mean)*(ptData_ch2[i] - ch2_mean);
    }
    ch1_sko = sqrt(ch1_sko / (datalength - 1));
    ch2_sko = sqrt(ch2_sko / (datalength - 1));

    ptX[loop_for_ptX(curpos)] = (ptData_ch1[curpos] - ch1_mean) / ch1_sko  - (ptData_ch2[curpos] - ch2_mean) / ch2_sko;
    ptCNSignal[curpos] = ( ptX[loop_for_ptX(curpos)] + ptCNSignal[loop(curpos - 1)] ) / 2.0;

    //----------------------------------------------------------------------------
    qreal outputValue = 0.0;
    for(quint16 i = 0; i < DIGITAL_FILTER_LENGTH ; i++)
    {
        outputValue += ptX[i];
    }
    pt_Xoutput[loop_for_ptX(curpos)] = outputValue / DIGITAL_FILTER_LENGTH;
    pt_Tempoutput[loop_on_two(curpos)] = pt_Xoutput[loop_for_ptX(curpos)] - pt_Xoutput[loop_for_ptX(curpos - (DIGITAL_FILTER_LENGTH - 1))];
    if( (pt_Tempoutput[0]*pt_Tempoutput[1]) < 0.0 )
    {
        m_zerocrossing = (++m_zerocrossing) % 2;
        if(m_zerocrossing == 0)
        {
            m_output *= -1.0;
        }
    }
    pt_Youtput[curpos] = m_output; // note, however, that pt_Youtput accumulate phase delay about DIGITAL_FILTER_LENGTH
    emit pt_YoutputWasUpdated(pt_Youtput, datalength);
    //----------------------------------------------------------------------------

    emit CNSignalWasUpdated(ptCNSignal, datalength);
    emit SignalActualValues(ptCNSignal[curpos], PCA_RAW_RGB(position, 0), PCA_RAW_RGB(position, 1), PCA_RAW_RGB(position, 2), HRfrequency, SNRE);

    curpos = (++curpos) % datalength; // for loop-like usage of ptData and the other arrays in this class
}

//----------------------------------------------------------------------------------------------------------

void QHarmonicProcessor::WriteToDataOneColor(unsigned long red, unsigned long green, unsigned long blue, unsigned long area, double time)
{
    qreal temp = 0.0;
    switch(m_channel)
    {
        case Red:
            temp = (qreal)red / area;
            break;
        case Green:
            temp = (qreal)green / area;
            break;
        case Blue:
            temp = (qreal)blue / area;
            break;
    }
    ch1_mean += (temp - ptData_ch1[curpos])/datalength;
    ptData_ch1[curpos] = temp;

    ptTime[curpos] = time;
    emit ptTimeWasUpdated(ptTime, datalength);

    qreal ch1_sko = 0.0;
    for (unsigned int i = 0; i < datalength; i++)
    {
        ch1_sko += (ptData_ch1[i] - ch1_mean)*(ptData_ch1[i] - ch1_mean);
    }
    ch1_sko = sqrt(ch1_sko / (datalength - 1));

    ptX[loop_for_ptX(curpos)] = (ptData_ch1[curpos] - ch1_mean)/ ch1_sko;
    ptCNSignal[curpos] = (ptX[loop_for_ptX(curpos)] + ptCNSignal[loop(curpos-1)]) / 2.0;

    //----------------------------------------------------------------------------
    qreal outputValue = 0.0;
    for(quint16 i = 0; i < DIGITAL_FILTER_LENGTH ; i++)
    {
        outputValue += ptX[i];
    }
    pt_Xoutput[loop_for_ptX(curpos)] = outputValue / DIGITAL_FILTER_LENGTH;
    pt_Tempoutput[loop_on_two(curpos)] = pt_Xoutput[loop_for_ptX(curpos)] - pt_Xoutput[loop_for_ptX(curpos - (DIGITAL_FILTER_LENGTH - 1))];
    if( (pt_Tempoutput[0]*pt_Tempoutput[1]) < 0.0 )
    {
        m_zerocrossing = (++m_zerocrossing) % 2;
        if(m_zerocrossing == 0)
        {
            m_output *= -1.0;
        }
    }
    pt_Youtput[curpos] = m_output; // note, however, that pt_Youtput accumulate phase delay about DIGITAL_FILTER_LENGTH
    emit pt_YoutputWasUpdated(pt_Youtput, datalength);
    //----------------------------------------------------------------------------

    m_accumulator += ptCNSignal[curpos];
    if( (--m_strobeValue) == 0)
    {
        pt_SlowPPG[m_pos] = m_accumulator / STROBE_FACTOR;
        emit SlowPPGWasUpdated(pt_SlowPPG, datalength);
        m_pos = (++m_pos) % datalength;
        m_strobeValue = STROBE_FACTOR;
        m_accumulator = 0.0;
    }

    emit CNSignalWasUpdated(ptCNSignal, datalength);
    emit SignalActualValues(ptCNSignal[curpos], ptData_ch1[curpos], ptData_ch1[curpos], ptData_ch1[curpos], HRfrequency, SNRE);

    curpos = (++curpos) % datalength; // for loop-like usage of ptData and the other arrays in this class
}

//----------------------------------------------------------------------------------------------------------

void QHarmonicProcessor::ComputeFrequency()
{
    qint16 temp_position = curpos - 1;
    qreal buffer_duration = 0.0; // for buffer duration accumulation without first time interval
    if(PCA_flag)
    {
        alglib::pcabuildbasis(PCA_RAW_RGB, bufferlength, 3, PCA_Info, PCA_Variance, PCA_Basis);
        if (PCA_Info == 1)
        {
            qreal mean0 = 0.0;
            qreal mean1 = 0.0;
            qreal mean2 = 0.0;
            for (unsigned int i = 0; i < bufferlength; i++)
            {
                mean0 += PCA_RAW_RGB(i,0);
                mean1 += PCA_RAW_RGB(i,1);
                mean2 += PCA_RAW_RGB(i,2);
            }
            mean0 /= bufferlength;
            mean1 /= bufferlength;
            mean2 /= bufferlength;

            qreal temp_sko = sqrt(PCA_Variance(0));
            qint16 start = loop_for_PCA(temp_position) - (bufferlength - 1); // has to be signed
            for (quint16 i = 0; i < bufferlength; i++)
            {
                quint16 pos = loop_for_PCA(start + i);
                ptDataForFFT[i] = ((PCA_RAW_RGB(pos,0) - mean0)*PCA_Basis(0,0) + (PCA_RAW_RGB(pos,1) - mean1)*PCA_Basis(1,0) + (PCA_RAW_RGB(pos,2) - mean2)*PCA_Basis(2,0)) / temp_sko;
                buffer_duration += ptTime[loop(temp_position - (bufferlength - 1) + i)];
            }
        }
        emit PCAProjectionWasUpdated(ptDataForFFT, bufferlength);
    }
    else
    {
        for (unsigned int i = 0; i < bufferlength; i++)
        {
            quint16 pos = loop(temp_position - (bufferlength - 1) + i);
            ptDataForFFT[i] = ptCNSignal[pos];
            buffer_duration += ptTime[pos];
        }
    }

    fftw_execute(m_plan); // Datas were prepared, now execute fftw_plan

    for (quint16 i = 0; i < (bufferlength/2 + 1); i++)
    {
        ptAmplitudeSpectrum[i] = ptSpectrum[i][0]*ptSpectrum[i][0] + ptSpectrum[i][1]*ptSpectrum[i][1];
    }
    emit SpectrumWasUpdated(ptAmplitudeSpectrum, bufferlength/2 + 1);

    quint16 bottom_bound = (quint16)(BOTTOM_LIMIT * buffer_duration / 1000.0);   // You should ensure that ( LOW_HR_LIMIT < discretization frequency / 2 )
    quint16 top_bound = (quint16)(TOP_LIMIT * buffer_duration / 1000.0);
    if(top_bound > (bufferlength / 2 + 1))
    {
        top_bound = bufferlength / 2 + 1;
    }
    quint16 index_of_maxpower = 0;
    qreal maxpower = 0.0;
    for (quint16 i = ( bottom_bound + HALF_INTERVAL ); i < ( top_bound - HALF_INTERVAL ); i++)
    {
        qreal temp_power = ptAmplitudeSpectrum[i];
        if ( maxpower < temp_power )
        {
            maxpower = temp_power;
            index_of_maxpower = i;
        }
    }
    /*-------------------------SNR estimation evaluation-----------------------*/
    qreal noise_power = 0.0;
    qreal signal_power = 0.0;
    for (quint16 i = bottom_bound; i < top_bound; i++)
    {
        if ( (i >= (index_of_maxpower - HALF_INTERVAL )) && (i <= (index_of_maxpower + HALF_INTERVAL)) )
        {
            signal_power += ptAmplitudeSpectrum[i];
        }
        else
        {
            noise_power += ptAmplitudeSpectrum[i];
        }
    }

    SNRE = 10 * log10( signal_power / noise_power );

    qreal power_multiplyed_by_index = 0.0;
    qreal power_of_first_harmonic = 0.0;
    for (qint16 i = (index_of_maxpower - HALF_INTERVAL); i <= (index_of_maxpower + HALF_INTERVAL); i++)
    {
        power_of_first_harmonic += ptAmplitudeSpectrum[i];
        power_multiplyed_by_index += i * ptAmplitudeSpectrum[i];
    }
    qreal bias = (qreal)index_of_maxpower - ( power_multiplyed_by_index / power_of_first_harmonic );
    bias = sqrt(bias * bias); // take abs of bias
    qreal resultWeight = (HALF_INTERVAL + 1 - bias)/(HALF_INTERVAL + 1);
    SNRE *= resultWeight * resultWeight * resultWeight * resultWeight; // make more multiplication to add more nonlinearity

    if(SNRE > SNR_TRESHOLD)
    {
        HRfrequency = (power_multiplyed_by_index / power_of_first_harmonic) * 60000.0 / buffer_duration;
        if((HRfrequency <= m_rightTreshold) && (HRfrequency >= m_leftThreshold))
            emit HRfrequencyWasUpdated(HRfrequency, SNRE, true);
        else
            emit HRfrequencyWasUpdated(HRfrequency, SNRE, false);
    }
    else
       emit TooNoisy(SNRE);
}

//----------------------------------------------------------------------------------------------------

void QHarmonicProcessor::set_PCA_flag(bool value)
{
    PCA_flag = value;
}

//----------------------------------------------------------------------------------------------------

void QHarmonicProcessor::switch_to_channel(color_channel value)
{
    m_channel = value;
}

//----------------------------------------------------------------------------------------------------

qreal QHarmonicProcessor::CountFrequency()
{
    qint16 position = curpos - 1; // delay on 1 count is critical valuable here
    quint16 watchDogCounter = 0;
    quint16 sign_changes = m_zerocrossingCounter;
    qreal temp_time = 0.0;

    while((pt_Youtput[loop(position)]*pt_Youtput[loop(position-1)] > 0.0) && (watchDogCounter < datalength))
    {
        position--;
        watchDogCounter++;
    }

    while((sign_changes > 0) && (watchDogCounter < datalength))
    {
        if(pt_Youtput[loop(position)]*pt_Youtput[loop(position-1)] < 0.0)
        {
            sign_changes--;
        }
        position--;
        watchDogCounter++;
        temp_time += ptTime[loop(position)];
    }

    HRfrequency = 60.0 * (m_zerocrossingCounter - 1) / ((temp_time - ptTime[loop(position)])/1000.0);
    emit HRfrequencyWasUpdated(HRfrequency,0.0,true);
    return HRfrequency;
}

//----------------------------------------------------------------------------------------------------

void QHarmonicProcessor::set_zerocrossingCounter(quint16 value)
{
    m_zerocrossingCounter = value;
}

//----------------------------------------------------------------------------------------------------

int QHarmonicProcessor::loadThresholds(const char *fileName, SexID sex, int age, TwoSideAlpha alpha)
{
    if( !QFile::exists( fileName ) ) {
        return FileExistanceError;
    }

    QFile file(fileName);
    if ( !file.open(QIODevice::ReadOnly) ) {
        return FileOpenError;
    }

    QXmlStreamReader reader(&file);
    QString desiredSex;
    switch(sex)   {
        case Male:
            desiredSex = "male";
            break;
        default:
            desiredSex = "female";
            break;
    }
    QString lowerPercentile;
    QString highestPercentile;
    switch(alpha) {
        case TwoPercents:
            lowerPercentile = "percentile1.0";
            highestPercentile = "percentile99.0";
            break;
        case FivePercents:
            lowerPercentile = "percentile2.5";
            highestPercentile = "percentile97.5";
            break;
        case TenPercents:
            lowerPercentile = "percentile5.0";
            highestPercentile = "percentile95.0";
            break;
        case TwentyPercents:
            lowerPercentile = "percentile10.0";
            highestPercentile = "percentile90.0";
            break;
        default:
            lowerPercentile = "percentile25.0";
            highestPercentile = "percentile75.0";
            break;
    }

    bool FoundSexSection = false;
    bool FoundAgeSection = false;
    bool FoundLowerPercentile = false;
    bool FoundHighestPercentile = false;
    bool ConversionResult1 = false;
    bool ConversionResult2 = false;
    qreal tempLeft = 0.0;
    qreal tempRight = 0.0;

    while(!reader.atEnd())   { // read to the end of xml file
        reader.readNext();
        if(reader.error()) {
            return ParseFailure;
        }
        else {
            if(reader.attributes().hasAttribute("type")) {
                if(reader.attributes().value("type") == desiredSex)
                    FoundSexSection = true;
            }
            if(FoundSexSection && reader.attributes().hasAttribute("agefrom")) {
                if( (age >= reader.attributes().value("agefrom").toInt()) && (age <= reader.attributes().value("ageto").toInt()))
                    FoundAgeSection = true;
            }
            if(FoundSexSection && FoundAgeSection) {
                if(reader.isStartElement() && (reader.name() == lowerPercentile))
                    FoundLowerPercentile = true;
                if(reader.isStartElement() && (reader.name() == highestPercentile))
                    FoundHighestPercentile = true;
            }
            if(FoundLowerPercentile && reader.isCharacters()) {
                tempLeft = reader.text().toDouble(&ConversionResult1);
                qWarning("leftTreshold: %f", tempLeft);
            }
            if(FoundLowerPercentile && reader.isEndElement() && (reader.name() == lowerPercentile))
                FoundLowerPercentile = false;

            if(FoundHighestPercentile && reader.isCharacters()) {
                tempRight = reader.text().toDouble(&ConversionResult2);
                qWarning("RightTreshold: %f", tempRight);
            }
            if(ConversionResult1 && ConversionResult2) {
                m_leftThreshold = tempLeft;
                m_rightTreshold = tempRight;
                return NoError;
            }
        }
    }
    qWarning("Xml parsing:can not find appropriate record in file!");
    return ReadError;
}

